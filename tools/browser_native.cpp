// native c++ browser automation: this tool owns a headless chromium child
// and drives it over the raw chrome devtools protocol (cdp) through
// httplib's websocket client. no python daemon, no second agent: the tool
// returns agent-grade context (observe: numbered interactive elements +
// visible text) and takes index/coordinate-based actions, so the caller is
// the agent loop.
//
// lifecycle: chromium lazily launches on the first call (persistent
// profile /tmp/clover-browser-profile, cookies survive relaunches), stays
// up between calls, idles out after 900s. a dead browser is relaunched
// transparently on the next call.
#include "../tool.h"
// must match main.cpp macro state: mismatched CPPHTTPLIB_OPENSSL_SUPPORT across
// TUs = different ClientImpl layouts (odr violation, heap corruption)
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "../httplib.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;
using httplib::ws::WebSocketClient;

static const char* PROFILE_DIR = "/tmp/clover-browser-profile";
static const int IDLE_SECS = 900;

static struct bstate {
    int pid = 0;
    int port = 0;
    int next_id = 1;
    time_t started = 0;
    time_t last_used = 0;
    std::unique_ptr<WebSocketClient> ws;
    bool seen_load = false;
} ST;

static void b_log(const std::string& m){
    std::fprintf(stderr, "[browser] %s\n", m.c_str());
}

static std::string cdp(const std::string& method, const json& params,
                       json& out, int timeout_s);

static std::string find_chrome(){
    const char* cands[] = {
        "/home/ubuntu/.cache/ms-playwright/chromium-1248/chrome-linux-arm64/chrome",
        "/home/ubuntu/.cache/ms-playwright/chromium-1248/chrome-linux/chrome",
        "/usr/bin/chromium", "/usr/bin/chromium-browser",
        "/usr/bin/google-chrome", nullptr };
    for (int i = 0; cands[i]; i++)
        if (access(cands[i], X_OK) == 0) return cands[i];
    const char* base = "/home/ubuntu/.cache/ms-playwright";
    DIR* d = opendir(base);
    if (!d) return "";
    struct dirent* e;
    std::string best;
    while ((e = readdir(d))){
        std::string n = e->d_name;
        if (n.rfind("chromium", 0) != 0 ||
            n.find("headless") != std::string::npos) continue;
        const char* sub[] = {"/chrome-linux-arm64/chrome", "/chrome-linux/chrome"};
        for (auto s : sub){
            std::string p = std::string(base) + "/" + n + s;
            if (access(p.c_str(), X_OK) == 0)
                if (best.empty() || p > best) best = p;
        }
    }
    closedir(d);
    return best;
}

static int pick_port(){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fd, (sockaddr*)&a, sizeof a) != 0){ close(fd); return 0; }
    sockaddr_in sa{}; socklen_t l = sizeof sa;
    getsockname(fd, (sockaddr*)&sa, &l);
    int p = ntohs(sa.sin_port);
    close(fd);
    return p;
}

static void kill_chrome(){
    if (ST.pid <= 0) return;
    kill(ST.pid, SIGTERM);
    for (int i = 0; i < 20; i++){
        int st;
        if (waitpid(ST.pid, &st, WNOHANG) == ST.pid) break;
        usleep(100000);
    }
    kill(ST.pid, SIGKILL);
    waitpid(ST.pid, nullptr, 0);
    ST.pid = 0;
}

static void teardown(){
    if (ST.ws){ try { ST.ws->close(); } catch (...) {} ST.ws.reset(); }
    kill_chrome();
    ST.port = 0;
    ST.seen_load = false;
}

static std::string launch(){
    teardown();
    std::string chrome = find_chrome();
    if (chrome.empty())
        return "error: no chromium binary found\n";
    int port = pick_port();
    if (!port) return "error: cannot pick a free port\n";
    std::string pstr = std::to_string(port);
    mkdir(PROFILE_DIR, 0700);
    std::vector<std::string> argstr = {
        chrome,
        "--remote-debugging-port=" + pstr,
        std::string("--user-data-dir=") + PROFILE_DIR,
        "--no-sandbox", "--disable-gpu", "--disable-dev-shm-usage",
        "--headless=new", "--no-first-run", "--no-default-browser-check",
        "--hide-scrollbars", "--remote-allow-origins=*",
        "--window-size=1280,800", "about:blank" };
    std::vector<char*> argv;
    for (auto& s : argstr) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    int devnull = open("/dev/null", O_WRONLY);
    pid_t pid = fork();
    if (pid < 0) return "error: fork failed\n";
    if (pid == 0){
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (devnull >= 0){ dup2(devnull, 1); dup2(devnull, 2); }
        execv(argv[0], argv.data());
        _exit(127);
    }
    if (devnull >= 0) close(devnull);
    ST.pid = pid;

    std::string page_ws;
    for (int i = 0; i < 100; i++){ // 20s
        {
            httplib::Client cli("127.0.0.1", port);
            cli.set_connection_timeout(1, 0);
            cli.set_read_timeout(1, 0);
            if (auto r = cli.Get("/json/list")){
                if (r->status == 200){
                    try {
                        json targets = json::parse(r->body);
                        for (auto& t : targets){
                            if (t.value("type", "") == "page" &&
                                t.contains("webSocketDebuggerUrl")){
                                page_ws = t["webSocketDebuggerUrl"]
                                    .get<std::string>();
                                break;
                            }
                        }
                    } catch (...) {}
                    if (!page_ws.empty()) break;
                }
            }
        }
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid){
            ST.pid = 0;
            return "error: chromium exited during startup\n";
        }
        usleep(200000);
    }
    if (page_ws.empty()){
        teardown();
        return "error: devtools endpoint never came up\n";
    }
    ST.ws = std::make_unique<WebSocketClient>(page_ws);
    ST.ws->set_read_timeout(1, 0);
    ST.ws->set_write_timeout(10, 0);
    if (!ST.ws->is_valid() || !ST.ws->connect()){
        teardown();
        return "error: cannot connect cdp websocket\n";
    }
    ST.port = port;
    ST.started = time(nullptr);
    ST.seen_load = false;
    json r;
    std::string e;
    if (!(e = cdp("Page.enable", json::object(), r, 10)).empty()) return e;
    if (!(e = cdp("Runtime.enable", json::object(), r, 10)).empty()) return e;
    b_log("chromium up pid " + std::to_string(ST.pid) +
          " port " + std::to_string(port));
    return "";
}

static std::string cdp(const std::string& method, const json& params,
                       json& out, int timeout_s){
    if (!ST.ws || !ST.ws->is_open())
        return "error: browser not running\n";
    int my = ST.next_id++;
    json m = {{"id", my}, {"method", method}};
    if (!params.is_null()) m["params"] = params;
    if (!ST.ws->send(m.dump())){
        teardown();
        return "error: cdp send failed (browser died), retry\n";
    }
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_s);
    std::string buf;
    while (std::chrono::steady_clock::now() < deadline){
        buf.clear();
        auto r = ST.ws->read(buf);
        if (r == httplib::ws::ReadResult::Text){
            try {
                json ev = json::parse(buf);
                if (ev.contains("id")){
                    if (ev["id"].get<int>() == my){
                        if (ev.contains("error"))
                            return "error: cdp " + method + ": " +
                                   ev["error"].value("message", "?") + "\n";
                        out = ev.value("result", json::object());
                        return "";
                    }
                } else if (ev.value("method", "") == "Page.loadEventFired"){
                    ST.seen_load = true;
                }
            } catch (...) {}
        } else if (r == httplib::ws::ReadResult::Fail){
            if (!ST.ws->is_open()){
                teardown();
                return "error: cdp connection closed, retry\n";
            }
        }
    }
    return "error: cdp " + method + " timed out\n";
}

static void wait_load(int timeout_s){
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_s);
    std::string buf;
    while (!ST.seen_load && std::chrono::steady_clock::now() < deadline){
        buf.clear();
        auto r = ST.ws->read(buf);
        if (r == httplib::ws::ReadResult::Text){
            try {
                json ev = json::parse(buf);
                if (ev.value("method", "") == "Page.loadEventFired")
                    ST.seen_load = true;
            } catch (...) {}
        } else if (r == httplib::ws::ReadResult::Fail && !ST.ws->is_open()){
            return;
        }
    }
}

static std::string eval_js(const std::string& expr, int timeout_s = 30){
    std::string wrapped = "(async()=>{try{const v=await eval(" +
        json(expr).dump() +
        ");return 'ok'+JSON.stringify(v===undefined?null:v);}" +
        "catch(e){return 'err'+String(e);}})()";
    json params = {{"expression", wrapped},
                   {"awaitPromise", true},
                   {"returnByValue", true}};
    json res;
    std::string e = cdp("Runtime.evaluate", params, res, timeout_s);
    if (!e.empty()) return e;
    if (res.contains("exceptionDetails"))
        return "error: js exception: " +
               res["exceptionDetails"].value("text", "?") + "\n";
    std::string v = res["result"].value("value", "");
    if (v.rfind("err", 0) == 0)
        return "error: js: " + v.substr(3).substr(0, 300) + "\n";
    std::string out = v.substr(2);
    if (out.size() > 200000) out = out.substr(0, 200000) + "...[truncated]";
    return out;
}

static bool ensure_running(std::string& err){
    err.clear();
    time_t now = time(nullptr);
    if (ST.pid > 0 && ST.ws && ST.ws->is_open()){
        if (now - ST.last_used > IDLE_SECS){
            b_log("idle shutdown");
            teardown();
        } else {
            ST.last_used = now;
            return true;
        }
    }
    err = launch();
    if (err.empty()) ST.last_used = time(nullptr);
    return err.empty();
}

// resolve numbered element n (set by the last observe) to {ok,tag,text,x,y,w,h}
static std::string resolve_n(int n, json& out){
    std::string js = "(function(){var q='[data-clover-n=\"" +
        std::to_string(n) + "\"]';" +
        "function dig(doc){var el=doc.querySelector(q);if(el)return el;" +
        "var fr=doc.querySelectorAll('iframe');" +
        "for(var i=0;i<fr.length;i++){try{var r=fr[i].contentDocument&&" +
        "dig(fr[i].contentDocument);if(r)return r;}catch(e){}}return null;}" +
        "var el=dig(document);" +
        "if(!el)return {ok:0,e:'stale index: observe again'};" +
        "try{el.scrollIntoView({block:'center'});}catch(e){}" +
        "var r=el.getBoundingClientRect();" +
        "return {ok:1,tag:el.tagName.toLowerCase()," +
        "text:(el.innerText||'').trim().slice(0,24)," +
        "x:+(r.left+r.width/2).toFixed(1),y:+(r.top+r.height/2).toFixed(1)," +
        "w:Math.round(r.width),h:Math.round(r.height)});})()";
    std::string v = eval_js(js);
    if (v.rfind("error", 0) == 0) return v;
    try { out = json::parse(v); }
    catch (...){ return "error: resolve parse failed\n"; }
    if (!out.value("ok", 0))
        return "error: " + out.value("e", "bad index") + "\n";
    return "";
}

static std::string mouse_event(const std::string& type, double x, double y,
                               int buttons, int click_count){
    json params = {{"type", type}, {"x", x}, {"y", y},
                   {"button", "left"}, {"buttons", buttons}};
    if (click_count > 0) params["clickCount"] = click_count;
    json res;
    return cdp("Input.dispatchMouseEvent", params, res, 10);
}

static std::string status_json(json& out){
    std::string u = "", t = "";
    if (ST.ws && ST.ws->is_open()){
        std::string v = eval_js(
            "({u:location.href,t:document.title})", 10);
        if (v.rfind("error", 0) != 0){
            try {
                json j = json::parse(v);
                u = j.value("u", ""); t = j.value("t", "");
            } catch (...) {}
        }
    }
    out = {{"running", ST.pid > 0},
           {"pid", ST.pid}, {"port", ST.port},
           {"uptime_s", ST.pid > 0 ? (int)(time(nullptr) - ST.started) : 0},
           {"url", u}, {"title", t}};
    return "";
}

static bool b64_decode(const std::string& in, std::string& out){
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.clear();
    out.reserve(in.size() * 3 / 4);
    int val = 0, valb = -8;
    for (char c : in){
        if (c == '\n' || c == '\r' || c == ' ') continue;
        if (c == '=') break;
        const char* p = strchr(T, c);
        if (!p) return false;
        val = (val << 6) + (int)(p - T);
        valb += 6;
        if (valb >= 0){
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

static const char* OBSERVE_JS =
"(function(){"
"var out=[], seen=new Set(), n=0, cap=200;"
"var all=document.querySelectorAll('[data-clover-n]');"
"for (var i=0;i<all.length;i++) all[i].removeAttribute('data-clover-n');"
"var SEL='a,button,input,select,textarea,option,summary,[role],[onclick],"
"[contenteditable],[tabindex],[draggable],li,tr,"
"svg path,svg circle,svg rect,svg polygon,svg ellipse,svg text,svg image';"
"function push(el, view){"
"  if (seen.has(el)||n>=cap) return;"
"  var r=el.getBoundingClientRect();"
"  if (r.width<=0&&r.height<=0) return;"
"  var cs=view.getComputedStyle(el);"
"  if (cs.visibility==='hidden'||cs.display==='none'||cs.pointerEvents==='none') return;"
"  seen.add(el); n++;"
"  try{el.setAttribute('data-clover-n',String(n));}catch(e){}"
"  var tag=el.tagName.toLowerCase();"
"  var type=(tag==='input')?(el.getAttribute('type')||'text'):'';"
"  var val=(tag==='input'||tag==='textarea'||tag==='select')?"
"    String(el.value||'').slice(0,40):'';"
"  var txt=(el.innerText||'').trim().replace(/\\s+/g,' ').slice(0,60);"
"  var aria=el.getAttribute('aria-label')||el.getAttribute('alt')||'';"
"  var cls=String(el.getAttribute('class')||'').split(/\\s+/)[0].slice(0,24);"
"  out.push({n:n, tag:tag+(type?':'+type:''), id:el.id||'',"
"    cls:cls, role:el.getAttribute('role')||'', text:txt||aria, val:val,"
"    x:Math.round(r.left), y:Math.round(r.top),"
"    w:Math.round(r.width), h:Math.round(r.height)});"
"}"
"function walk(doc){"
"  var view=doc.defaultView; if(!view) return;"
"  var els; try{els=doc.querySelectorAll('*');}catch(e){return;}"
"  for (var i=0;i<els.length&&n<cap;i++){"
"    var el=els[i]; var hit=false;"
"    try{hit=el.matches(SEL);}catch(e){hit=false;}"
"    if (!hit){"
"      var cs=view.getComputedStyle(el);"
"      if (cs.cursor!=='pointer') continue;"
"    }"
"    push(el, view);"
"  }"
"  var fr=doc.querySelectorAll('iframe');"
"  for (var j=0;j<fr.length;j++){"
"    try{ if(fr[j].contentDocument) walk(fr[j].contentDocument); }catch(e){}"
"  }"
"}"
"walk(document);"
"var text=document.body?document.body.innerText:'';"
"return {url:location.href,title:document.title,count:n,"
"  elements:out,text:text.slice(0,4000),textlen:text.length};"
"})()";

static std::string render_observe(const std::string& raw){
    json j;
    try { j = json::parse(raw); }
    catch (...){ return "error: observe result did not parse\n"; }
    std::string out;
    out += "url: " + j.value("url", "") + "\n";
    out += "title: " + j.value("title", "") + "\n";
    out += "elements: " + std::to_string(j.value("count", 0)) +
           " (act by [n])\n";
    for (auto& e : j.value("elements", json::array())){
        out += "[" + std::to_string(e.value("n", 0)) + "] " +
               e.value("tag", "");
        if (!e.value("id", "").empty()) out += "#" + e.value("id", "");
        if (!e.value("cls", "").empty()) out += "." + e.value("cls", "");
        if (!e.value("role", "").empty()) out += " role=" + e.value("role", "");
        if (!e.value("text", "").empty())
            out += " '" + e.value("text", "") + "'";
        if (!e.value("val", "").empty())
            out += " val='" + e.value("val", "") + "'";
        out += " @(" + std::to_string(e.value("x", 0)) + "," +
               std::to_string(e.value("y", 0)) + ") " +
               std::to_string(e.value("w", 0)) + "x" +
               std::to_string(e.value("h", 0)) + "\n";
    }
    std::string text = j.value("text", "");
    int textlen = j.value("textlen", (int)text.size());
    out += "--- page text (" + std::to_string(textlen) + " chars";
    if (textlen > (int)text.size()) out += ", truncated";
    out += ") ---\n" + text + "\n";
    return out;
}

static std::string tool_browser(const json& args, tool_context&){
    std::string action = args.value("action", "");
    std::string err;

    if (action == "close"){
        if (ST.pid > 0){ teardown(); return "ok: browser closed\n"; }
        return "ok: browser not running\n";
    }
    if (action == "restart"){
        teardown();
        err = launch();
        if (!err.empty()) return err;
        json s; status_json(s);
        return "restarted: " + s.dump() + "\n";
    }
    if (!ensure_running(err)) return err;

    if (action == "status"){
        json s; status_json(s);
        return s.dump() + "\n";
    }

    if (action == "navigate"){
        std::string url = args.value("url", "");
        if (url.empty()) return "error: missing url\n";
        if (url.find("://") == std::string::npos) url = "http://" + url;
        json res;
        ST.seen_load = false;
        err = cdp("Page.navigate", {{"url", url}}, res, 30);
        if (!err.empty()) return err;
        if (res.contains("errorText") && !res["errorText"].is_null())
            return "error: navigate: " +
                   res["errorText"].get<std::string>() + "\n";
        wait_load(15);
        json s; status_json(s);
        return "navigated. url: " + s.value("url", "") +
               " title: " + s.value("title", "") + "\n";
    }

    if (action == "observe"){
        std::string v = eval_js(OBSERVE_JS);
        if (v.rfind("error", 0) == 0) return v;
        return render_observe(v);
    }

    if (action == "click"){
        if (!args.contains("n")) return "error: click needs n\n";
        int n = args["n"].get<int>();
        json el;
        err = resolve_n(n, el);
        if (!err.empty()) return err;
        double x = el.value("x", 0.0), y = el.value("y", 0.0);
        if (!(err = mouse_event("mousePressed", x, y, 1, 1)).empty()) return err;
        if (!(err = mouse_event("mouseReleased", x, y, 0, 1)).empty()) return err;
        usleep(150000);
        json s; status_json(s);
        return "clicked [" + std::to_string(n) + "] " + el.value("tag", "") +
               " '" + el.value("text", "") + "' url: " + s.value("url", "") + "\n";
    }

    if (action == "click_xy"){
        double x = args.value("x", 0.0), y = args.value("y", 0.0);
        if (!(err = mouse_event("mousePressed", x, y, 1, 1)).empty()) return err;
        if (!(err = mouse_event("mouseReleased", x, y, 0, 1)).empty()) return err;
        usleep(150000);
        return "clicked (" + std::to_string(x) + "," +
               std::to_string(y) + ")\n";
    }

    if (action == "type"){
        if (!args.contains("n") || !args.contains("text"))
            return "error: type needs n and text\n";
        int n = args["n"].get<int>();
        std::string text = args["text"].get<std::string>();
        json el;
        err = resolve_n(n, el);
        if (!err.empty()) return err;
        if (!(err = mouse_event("mousePressed", el.value("x", 0.0),
                                el.value("y", 0.0), 1, 1)).empty()) return err;
        if (!(err = mouse_event("mouseReleased", el.value("x", 0.0),
                                el.value("y", 0.0), 0, 1)).empty()) return err;
        {
            std::string js = "(function(){var el=document.querySelector('"
                "[data-clover-n=\"" + std::to_string(n) + "\"]');"
                "if(el&&(el.tagName==='INPUT'||el.tagName==='TEXTAREA'))"
                "el.value='';"
                "return 1;})()";
            eval_js(js);
        }
        json res;
        err = cdp("Input.insertText", {{"text", text}}, res, 10);
        if (!err.empty()) return err;
        std::string js = "(function(){var el=document.querySelector('"
            "[data-clover-n=\"" + std::to_string(n) + "\"]');"
            "return el?String(el.value!==undefined?el.value:''"
            ".substr(0,60)):'';})()";
        std::string v = eval_js(js);
        std::string nowv = (v.rfind("error", 0) == 0) ? "?" : v;
        if (!nowv.empty() && nowv[0] == '"')
            try { nowv = json::parse(nowv).get<std::string>(); } catch (...) {}
        return "typed into [" + std::to_string(n) + "] " + el.value("tag", "") +
               ". now: " + nowv + "\n";
    }

    if (action == "press"){
        std::string key = args.value("key", "Enter");
        struct K { const char* name; const char* key_; const char* code; int vk; };
        static const K keys[] = {
            {"enter", "Enter", "Enter", 13},
            {"tab", "Tab", "Tab", 9},
            {"escape", "Escape", "Escape", 27},
            {"backspace", "Backspace", "Backspace", 8},
            {"delete", "Delete", "Delete", 46},
            {"arrowup", "ArrowUp", "ArrowUp", 38},
            {"arrowdown", "ArrowDown", "ArrowDown", 40},
            {"arrowleft", "ArrowLeft", "ArrowLeft", 37},
            {"arrowright", "ArrowRight", "ArrowRight", 39},
            {"space", " ", "Space", 32},
            {"home", "Home", "Home", 36},
            {"end", "End", "End", 35},
            {"pageup", "PageUp", "PageUp", 33},
            {"pagedown", "PageDown", "PageDown", 34},
        };
        std::string lo = key;
        for (auto& c : lo) c = (char)tolower((unsigned char)c);
        const K* k = nullptr;
        for (auto& kk : keys) if (lo == kk.name){ k = &kk; break; }
        if (!k) return "error: unsupported key\n";
        json res;
        json down = {{"type", "keyDown"}, {"key", k->key_},
                     {"code", k->code},
                     {"windowsVirtualKeyCode", k->vk},
                     {"nativeVirtualKeyCode", k->vk}};
        if (k->vk == 32) down["text"] = " ";
        if (!(err = cdp("Input.dispatchKeyEvent", down, res, 10)).empty())
            return err;
        json up = down;
        up["type"] = "keyUp";
        up.erase("text");
        if (!(err = cdp("Input.dispatchKeyEvent", up, res, 10)).empty())
            return err;
        return "pressed " + key + "\n";
    }

    if (action == "hover"){
        if (!args.contains("n")) return "error: hover needs n\n";
        int n = args["n"].get<int>();
        json el;
        err = resolve_n(n, el);
        if (!err.empty()) return err;
        double x = el.value("x", 0.0), y = el.value("y", 0.0);
        if (!(err = mouse_event("mouseMoved", x - 5, y - 5, 0, 0)).empty()) return err;
        if (!(err = mouse_event("mouseMoved", x, y, 0, 0)).empty()) return err;
        usleep(200000);
        return "hovered [" + std::to_string(n) + "]\n";
    }

    auto do_drag = [&](double x1, double y1, double x2, double y2,
                       int steps) -> std::string {
        if (!(err = mouse_event("mousePressed", x1, y1, 1, 1)).empty()) return err;
        for (int i = 1; i <= steps; i++){
            double x = x1 + (x2 - x1) * i / steps;
            double y = y1 + (y2 - y1) * i / steps;
            if (!(err = mouse_event("mouseMoved", x, y, 1, 0)).empty()) return err;
            usleep(16000);
        }
        if (!(err = mouse_event("mouseReleased", x2, y2, 0, 1)).empty()) return err;
        usleep(150000);
        return "";
    };

    if (action == "drag"){
        if (!args.contains("from_n") || !args.contains("to_n"))
            return "error: drag needs from_n and to_n\n";
        json a, b;
        err = resolve_n(args["from_n"].get<int>(), a);
        if (!err.empty()) return err;
        err = resolve_n(args["to_n"].get<int>(), b);
        if (!err.empty()) return err;
        err = do_drag(a.value("x", 0.0), a.value("y", 0.0),
                      b.value("x", 0.0), b.value("y", 0.0), 12);
        if (!err.empty()) return err;
        return "dragged [" + std::to_string(args["from_n"].get<int>()) +
               "] onto [" + std::to_string(args["to_n"].get<int>()) + "]\n";
    }

    if (action == "drag_xy"){
        if (!args.contains("x1") || !args.contains("y1") ||
            !args.contains("x2") || !args.contains("y2"))
            return "error: drag_xy needs x1,y1,x2,y2\n";
        err = do_drag(args.value("x1", 0.0), args.value("y1", 0.0),
                      args.value("x2", 0.0), args.value("y2", 0.0), 12);
        if (!err.empty()) return err;
        return "dragged (" + std::to_string(args.value("x1", 0.0)) + "," +
               std::to_string(args.value("y1", 0.0)) + ") -> (" +
               std::to_string(args.value("x2", 0.0)) + "," +
               std::to_string(args.value("y2", 0.0)) + ")\n";
    }

    if (action == "scroll"){
        int dy = args.value("dy", 300);
        std::string js = "window.scrollBy(0," + std::to_string(dy) +
            ");({y:window.scrollY,h:document.body.scrollHeight})";
        std::string v = eval_js(js);
        if (v.rfind("error", 0) == 0) return v;
        return "scrolled " + std::to_string(dy) + ": " + v + "\n";
    }

    if (action == "read"){
        std::string sel = args.value("selector", "");
        if (sel.empty()) return "error: read needs selector\n";
        std::string js = "(function(){var e=document.querySelector(" +
            json(sel).dump() +");var t=e?e.innerText:'';return t.slice(0,100000);})()";
        std::string v = eval_js(js);
        if (v.rfind("error", 0) == 0) return v;
        try { return json::parse(v).get<std::string>() + "\n"; }
        catch (...){ return v + "\n"; }
    }

    if (action == "eval"){
        std::string js = args.value("js", "");
        if (js.empty()) return "error: eval needs js\n";
        std::string v = eval_js(js, 60);
        if (v.rfind("error", 0) == 0) return v;
        if (v == "null") return "null\n";
        return v + "\n";
    }

    if (action == "screenshot"){
        bool full = args.value("full", true);
        json params = {{"format", "png"},
                       {"captureBeyondViewport", full}};
        json res;
        err = cdp("Page.captureScreenshot", params, res, 30);
        if (!err.empty()) return err;
        std::string png;
        if (!b64_decode(res.value("data", ""), png))
            return "error: screenshot decode failed\n";
        std::string path = args.value("path", "");
        if (path.empty())
            path = "/tmp/browser-shot-" + std::to_string(time(nullptr)) + ".png";
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return "error: cannot write " + path + "\n";
        fwrite(png.data(), 1, png.size(), f);
        fclose(f);
        return "screenshot saved: " + path + " (" +
               std::to_string(png.size()) + " bytes)\n";
    }

    return "error: unknown action '" + action +
           "'. actions: status, navigate, observe, click, click_xy, type, "
           "press, hover, drag, drag_xy, scroll, read, eval, screenshot, "
           "restart, close\n";
}

// the python-daemon browser tool is gone; this is THE browser tool.
REGISTER_TOOL_DESC("browser", tool_browser,
R"(browser(action, ...): native c++ browser automation (owns a headless chromium child, driven over raw cdp; no daemon, no second agent - the caller IS the agent loop)
-owns a headless chromium child, driven over raw cdp; no daemon, no second agent - the caller IS the agent loop
-actions: status, navigate(url), observe(), click(n), click_xy(x,y), type(n,text), press(key), hover(n), drag(from_n,to_n), drag_xy(x1,y1,x2,y2), scroll(dy), read(selector), eval(js), screenshot(path?,full?), restart, close
-workflow: navigate, then observe -> numbered interactive elements (tag, id, class, text, value, position, size) + page text (capped 4000 chars). act by index n. indexes refer to the last observe; stale index errors mean observe again
-svg shapes and cursor:pointer elements are numbered with geometry; for computed positions (pie wedges, canvas) use click_xy/drag_xy with coordinates derived from observed geometry
-same-origin iframes walked automatically; cookies persist in the profile dir; chromium lazily starts on first call, idles out after 900s (relaunch transparent)
-for plain text search use web_search; use this tool for javascript pages, clicking, forms, and session state)");
