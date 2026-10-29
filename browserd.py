#!/home/ubuntu/browserenv/bin/python
# clover browser daemon: long-lived headless chromium over a unix socket.
# protocol: one JSON request per line, one JSON response per line.
# one shared page for all clients; cookies persist via the profile dir.
import json, os, socket, sys, time, signal, traceback

from playwright.sync_api import sync_playwright, Error as PWError

SOCK_PATH = "/tmp/browserd.sock"
PROFILE   = os.path.expanduser("~/browser-profile")
IDLE_SECS = 600          # exit when no client connects for this long
MAX_TEXT  = 200_000       # cap on extracted text/html
MAX_EVAL  = 200_000       # cap on eval results
START     = time.time()

B = None  # Browser instance, rebuilt on crash

class Browser:
    def __init__(self):
        self.pw  = sync_playwright().start()
        self.ctx = self.pw.chromium.launch_persistent_context(
            PROFILE, headless=True)
        self.page = (self.ctx.pages[0] if self.ctx.pages
                     else self.ctx.new_page())

    def close(self):
        for f in (lambda: self.ctx.close(), lambda: self.pw.stop()):
            try: f()
            except Exception: pass


def log(msg):
    print("[browserd %s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def reset_browser():
    global B
    if B:
        B.close()
        B = None


def handle(req):
    global B
    act = str(req.get("action", "")).strip()
    to  = min(max(int(req.get("timeout", 30)), 1), 120)
    if B is None:
        reset_browser()
        B = Browser()
        log("browser (re)launched")
    pg = B.page

    if act == "status":
        return {"ok": True, "url": pg.url, "title": pg.title(),
                "pid": os.getpid(), "uptime": int(time.time() - START)}

    if act == "navigate":
        url = str(req.get("url", "")).strip()
        if not url:
            return {"ok": False, "error": "missing url"}
        if "://" not in url:
            url = "https://" + url
        r = pg.goto(url, timeout=to * 1000, wait_until="domcontentloaded")
        return {"ok": True, "url": pg.url,
                "status": (r.status if r else None), "title": pg.title()}

    if act == "extract":
        sel = req.get("selector")
        if sel:
            el = pg.query_selector(sel)
            if el is None:
                return {"ok": False, "error": "selector not found: %s" % sel}
            t = el.inner_text(timeout=to * 1000)
        else:
            t = pg.evaluate(
                "() => document.body ? document.body.innerText : ''")
        if len(t) > MAX_TEXT:
            t = t[:MAX_TEXT] + "\n...[truncated %d chars]" % (len(t) - MAX_TEXT)
        return {"ok": True, "text": t, "url": pg.url, "title": pg.title()}

    if act == "links":
        n = min(int(req.get("limit", 100)), 500)
        ls = pg.evaluate("""(n) => {
            const out = [];
            for (const a of document.querySelectorAll('a[href]')) {
                out.push({href: a.href,
                          text: (a.innerText || '').trim().slice(0, 80)});
                if (out.length >= n) break;
            }
            return out; }""", n)
        return {"ok": True, "count": len(ls), "links": ls}

    if act == "click":
        sel = str(req.get("selector", "")).strip()
        if not sel:
            return {"ok": False, "error": "missing selector"}
        el = pg.query_selector(sel)
        if el is None:
            return {"ok": False, "error": "selector not found: %s" % sel}
        el.click(timeout=to * 1000)
        pg.wait_for_load_state("domcontentloaded", timeout=5000)
        return {"ok": True, "url": pg.url, "title": pg.title()}

    if act == "type":
        sel = str(req.get("selector", "")).strip()
        txt = req.get("text", "")
        if not sel:
            return {"ok": False, "error": "missing selector"}
        el = pg.query_selector(sel)
        if el is None:
            return {"ok": False, "error": "selector not found: %s" % sel}
        el.fill(str(txt), timeout=to * 1000)
        return {"ok": True}

    if act == "eval":
        js = str(req.get("js", "")).strip()
        if not js:
            return {"ok": False, "error": "missing js"}
        r = pg.evaluate("""(js) => {
            try {
                const v = eval(js);
                const s = JSON.stringify(v === undefined ? null : v);
                return 'ok' + (s === undefined ? 'null' : s);
            } catch (e) { return 'err' + String(e); }
        }""", js)
        if r.startswith("err"):
            return {"ok": False, "error": r[3:][:300]}
        out = r[2:]
        if len(out) > MAX_EVAL:
            out = out[:MAX_EVAL] + "...[truncated]"
        return {"ok": True, "result": out}

    if act == "html":
        h = pg.content()
        if len(h) > MAX_TEXT:
            h = h[:MAX_TEXT] + "...[truncated %d chars]" % (len(h) - MAX_TEXT)
        return {"ok": True, "html": h, "url": pg.url}

    if act == "screenshot":
        path = req.get("path") or "/tmp/browser-shot-%d.png" % int(time.time())
        pg.screenshot(path=str(path),
                      full_page=bool(req.get("full", True)),
                      timeout=to * 1000)
        return {"ok": True, "path": str(path),
                "bytes": os.path.getsize(path)}

    if act == "back":
        pg.go_back(timeout=to * 1000)
        return {"ok": True, "url": pg.url, "title": pg.title()}

    if act == "forward":
        pg.go_forward(timeout=to * 1000)
        return {"ok": True, "url": pg.url, "title": pg.title()}

    if act == "reset":
        pg.goto("about:blank")
        return {"ok": True, "url": "about:blank"}

    return {"ok": False, "error": "unknown action: %r" % act}


def die(sig, frm):
    log("signal exit")
    reset_browser()
    try: os.unlink(SOCK_PATH)
    except OSError: pass
    sys.exit(0)


def main():
    signal.signal(signal.SIGTERM, die)
    signal.signal(signal.SIGINT, die)
    if os.path.exists(SOCK_PATH):
        os.unlink(SOCK_PATH)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCK_PATH)
    os.chmod(SOCK_PATH, 0o600)
    srv.listen(8)
    srv.settimeout(IDLE_SECS)
    log("listening on %s (idle exit %ds)" % (SOCK_PATH, IDLE_SECS))
    while True:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            log("idle for %ds, exiting" % IDLE_SECS)
            break
        try:
            conn.settimeout(300)
            buf = b""
            while True:
                while b"\n" not in buf:
                    chunk = conn.recv(65536)
                    if not chunk:
                        raise ConnectionError("client closed")
                    buf += chunk
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    resp = handle(json.loads(line))
                except PWError as e:
                    reset_browser()
                    resp = {"ok": False, "error": "browser error (reset), "
                            "retry: %s" % str(e)[:250]}
                except Exception as e:
                    resp = {"ok": False, "error": "%s: %s"
                            % (type(e).__name__, str(e)[:250])}
                conn.sendall(json.dumps(resp).encode() + b"\n")
        except Exception as e:
            log("conn: %s" % str(e)[:150])
        finally:
            try: conn.close()
            except OSError: pass
    reset_browser()
    try: os.unlink(SOCK_PATH)
    except OSError: pass


if __name__ == "__main__":
    main()
