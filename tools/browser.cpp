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
#include <set>
#include <map>
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
    std::vector<json> selmap; // index-> {n,backend,role,name,tag,x,y} from last observe
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
    // headed-under-xvfb mode: if Xvfb exists and CLOVER_HEADLESS is not "1",
    // run a real (non-headless) chrome under a virtual display. bot walls
    // fingerprint headless chromium hard; a headed instance under xvfb
    // behaves like a desktop browser to the page.
    static int xvfb_mode = -1;
    if (xvfb_mode < 0){
        const char* env = getenv("CLOVER_HEADLESS");
        xvfb_mode = (env && strcmp(env, "1") == 0) ? 0 : 1;
        if (xvfb_mode){
            if (access("/usr/bin/Xvfb", X_OK) != 0) xvfb_mode = 0;
        }
    }
    if (xvfb_mode){
        // ensure the virtual display is up before chrome needs it
        if (system("pgrep -x Xvfb >/dev/null 2>&1 || "
                    "(setsid nohup Xvfb :99 -screen 0 1280x800x24 "
                    " >/dev/null 2>&1 & sleep 1)") != 0){}
    }
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
        "--enable-unsafe-swiftshader",
        "--no-first-run", "--no-default-browser-check",
        "--hide-scrollbars", "--remote-allow-origins=*",
        "--disable-blink-features=AutomationControlled",
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
        if (xvfb_mode) setenv("DISPLAY", ":99", 1);
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
    // stealth init script: runs before every page script. hides the
    // headless/webdriver tells that bot-walls fingerprint (same script the
    // old python daemon used, ported to cdp).
    static const char* STEALTH_JS =
        "Object.defineProperty(navigator,'webdriver',"
        "{get:()=>undefined});"
        "window.chrome=window.chrome||{runtime:{}};"
        "(()=>{const ua=navigator.userAgent;"
        "if(ua&&ua.indexOf('HeadlessChrome')>=0)"
        "Object.defineProperty(navigator,'userAgent',"
        "{get:()=>ua.replace('HeadlessChrome','Chrome')});})();";
    {
        json r2;
        std::string e;
        if (!(e = cdp("Page.addScriptToEvaluateOnNewDocument",
                      {{"source", STEALTH_JS}}, r2, 10)).empty()) return e;
    }
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
// ---- AX-tree layer: resolve a CDP backendNodeId to live viewport coords ----
// the clean action path: DOM.resolveNode -> runtime objectId, then call
// getBoundingClientRect on it. no DOM-attribute injection, nothing goes stale.
static std::string resolve_backend(long long backend,
    double& x, double& y, std::string& tag, std::string& text,
    std::string& objectId){
    json r; std::string e;
    if (!(e=cdp("DOM.enable", json::object(), r, 10)).empty()) return e;
    json rr;
    e=cdp("DOM.resolveNode", {{"backendNodeId", backend}}, rr, 10);
    if (!e.empty()) return e;
    if (!rr.contains("object") || !rr["object"].contains("objectId"))
        return "error: element not resolvable (page changed - observe again)\n";
    objectId=rr["object"]["objectId"].get<std::string>();
    // callFunctionOn invokes this with `this` = the resolved element, so the
    // body must be a plain function, NOT an IIFE (an IIFE rebinds `this`).
    static const std::string JS_RECT=R"JS(
function(){try{this.scrollIntoView({block:'center'});}catch(e){}
var r=this.getBoundingClientRect();
return {x:r.left+r.width/2,y:r.top+r.height/2,w:r.width,h:r.height,
t:this.tagName?this.tagName.toLowerCase():'',
txt:(this.innerText||this.value||'').trim().slice(0,24)};}
)JS";
    json cr;
    e=cdp("Runtime.callFunctionOn", {{"objectId", objectId},
        {"functionDeclaration", JS_RECT}, {"returnByValue", true}}, cr, 10);
    if (!e.empty()) return e;
    if (cr.contains("exceptionDetails"))
        return "error: resolve failed: "+cr["exceptionDetails"].value("text","?")+"\n";
    json v=cr["result"]["value"];
    x=v.value("x",0.0); y=v.value("y",0.0);
    tag=v.value("t",""); text=v.value("txt","");
    if (v.value("w",0.0)<=0 && v.value("h",0.0)<=0)
        return "error: element has no size (not visible)\n";
    return "";
}

static std::string resolve_n_ax(int n, double& x, double& y,
    std::string& tag, std::string& text, std::string& objectId){
    if (n<=0 || n>=(int)ST.selmap.size())
        return "error: stale index ["+std::to_string(n)+"] - observe again\n";
    long long b=ST.selmap[n-1].value("backend", -1ll);
    return resolve_backend(b, x, y, tag, text, objectId);
}

static const std::string JS_CLEAR=R"JS(
function(){if(this.tagName==='INPUT'||this.tagName==='TEXTAREA')this.value='';
if(this.isContentEditable)this.innerHTML='';return 1;}
)JS";

static void clear_selection(long long backend, std::string& objectId){
    json r; std::string e;
    cdp("DOM.enable", json::object(), r, 10);
    json rr;
    if (cdp("DOM.resolveNode", {{"backendNodeId", backend}}, rr, 10).empty() &&
        rr.contains("object") && rr["object"].contains("objectId")){
        objectId=rr["object"]["objectId"].get<std::string>();
        json cr;
        cdp("Runtime.callFunctionOn", {{"objectId", objectId},
            {"functionDeclaration", JS_CLEAR}, {"returnByValue", true}}, cr, 10);
    }
}

// AX properties are {name, value:{type, value, ...}}. extract the leaf and
// stringify it (bools come through as true/false).
static std::string ax_prop_leaf(const json& p){
    json v = p.contains("value") ? p["value"] : json();
    if (v.is_object() && v.contains("value")) v = v["value"];
    if (v.is_boolean()) return v.get<bool>()?"true":"false";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return std::to_string(v.get<double>());
    return "";
}

// ---- interactivity: roles that mean the node is a control a human can act on ----
static bool is_interactive_role(const std::string& role, const json& props){
    static const std::set<std::string> R={
        "button","link","menuitem","menuitemcheckbox","menuitemradio","option",
        "radio","checkbox","tab","textbox","combobox","listbox","slider",
        "spinbutton","searchbox","switch","treeitem","gridcell","row","menu",
        "menubar","tablist"};
    if (role=="presentation" || role=="none") return false;
    if (R.count(role)) return true;
    // focusable/editable props make otherwise-generic nodes interactive
    for (auto& p : props){
        std::string pn=p.value("name",""), pv=ax_prop_leaf(p);
        if ((pn=="focusable"||pn=="editable"||pn=="settable") && pv=="true")
            return true;
    }
    return false;
}

static std::string state_from_props(const json& props){
    std::string s;
    for (auto& p : props){
        std::string pn=p.value("name",""), pv=ax_prop_leaf(p);
        if (pv!="true") continue;
        if (pn=="disabled") s+="disabled ";
        else if (pn=="checked") s+="checked ";
        else if (pn=="expanded") s+="expanded ";
        else if (pn=="selected") s+="selected ";
        else if (pn=="focused") s+="focused ";
    }
    return s;
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
            return "error: navigate: "+res["errorText"].get<std::string>()+"\n";
        wait_load(15);
        ST.selmap.clear();
        json s; status_json(s);
        return "navigated. url: "+s.value("url","")+" title: "+s.value("title","")+"\n";
    }

    if (action == "go_back"){ eval_js("history.back()"); usleep(400000); ST.selmap.clear(); return "went back\n"; }
    if (action == "go_forward"){ eval_js("history.forward()"); usleep(400000); ST.selmap.clear(); return "went forward\n"; }
    if (action == "reload"){ eval_js("location.reload()"); wait_load(10); ST.selmap.clear(); return "reloaded\n"; }

    if (action == "observe"){
        // accessibility-tree view of the page: index only interactive, visible
        // nodes; render role + accessible name. this is the semantic snapshot an
        // agent should act on (the method browser-use / stagehand converge on),
        // not a list of raw css tags.
        json r; std::string e;
        if (!(e=cdp("Accessibility.enable", json::object(), r, 10)).empty()) return e;
        json ax;
        if (!(e=cdp("Accessibility.getFullAXTree", json::object(), ax, 30)).empty()) return e;
        json snap;
        if (!(e=cdp("DOM.enable", json::object(), r, 10)).empty()) return e;
        e=cdp("DOMSnapshot.captureSnapshot",
              {{"computedStyles", json::array()}, {"includeDOMRects", true},
               {"includePaintOrder", true}}, snap, 30);
        if (!e.empty()) return e;
        // backendDOMNodeId -> viewport rect, from the snapshot layout. the layout
        // stores per-node rects indexed by layout.nodeIndex[i] (a document node
        // index); doc.nodes maps node-index -> backendDOMNodeId in parallel arrays.
        std::map<long long, json> geo;
        for (auto& doc : snap["documents"]){
            if (!doc.contains("nodes") || !doc.contains("layout")) continue;
            auto& N=doc["nodes"]; auto& lay=doc["layout"];
            if (!N.contains("backendNodeId") || !lay.contains("nodeIndex") || !lay.contains("bounds")) continue;
            auto& bs=N["backendNodeId"]; auto& lni=lay["nodeIndex"]; auto& bounds=lay["bounds"];
            // node-index -> layout entry
            std::map<long long, long long> ni2li;
            for (size_t i=0;i<lni.size();i++)
                if (lni[i].is_number())
                    ni2li[(long long)lni[i].get<double>()]=(long long)i;
            for (size_t i=0;i<bs.size();i++){
                if (!bs[i].is_number()) continue;
                long long b=(long long)bs[i].get<double>();
                auto it=ni2li.find((long long)i);
                if (it!=ni2li.end()){
                    long long li=it->second;
                    if (li>=0 && (size_t)li<bounds.size() && bounds[li].is_array() && bounds[li].size()>=4)
                        geo[b]=bounds[li];
                }
            }
        }
        std::vector<json> items; int n=0;
        for (auto& nd : ax["nodes"]){
            if (nd.value("ignored", false)) continue;
            std::string role=nd.value("role", json::object()).value("value", "");
            std::string name=nd.value("name", json::object()).value("value", "");
            std::string desc=nd.value("description", json::object()).value("value", "");
            std::string val=nd.value("value", json::object()).value("value", "");
            if (role=="RootWebArea" || role=="GenericContainer" || role=="group" ||
                role=="section" || role=="application" || role=="document") continue;
            json props=nd.value("properties", json::array());
            long long b=nd.value("backendDOMNodeId", -1ll);
            if (b<0) continue;
            if (!is_interactive_role(role, props)) continue;
            std::string state=state_from_props(props);
            if (state.find("disabled ")!=std::string::npos) continue; // dead control
            json it;
            it["role"]=role; it["name"]=name; it["desc"]=desc; it["val"]=val;
            it["state"]=state; it["backend"]=b;
            auto g=geo.find(b);
            bool visible=false;
            if (g!=geo.end() && g->second.is_array() && g->second.size()>=4){
                double w=g->second[2].get<double>(), h=g->second[3].get<double>();
                if (w>0 && h>0) visible=true;
            }
            if (!visible) continue;
            items.push_back(it); n++;
            if (n>=300) break;
        }
        ST.selmap.clear();
        for (size_t i=0;i<items.size();i++){ items[i]["n"]=(int)(i+1); ST.selmap.push_back(items[i]); }
        std::string text=eval_js("document.body?document.body.innerText.slice(0,4000):''", 10);
        if (text.rfind("error",0)==0) text="";
        std::string out;
        json s; status_json(s);
        out+="url: "+s.value("url","")+"\n";
        out+="title: "+s.value("title","")+"\n";
        out+="interactive: "+std::to_string(items.size())+" (act by [n])\n";
        for (auto& it : items){
            out+="["+std::to_string((int)it["n"])+"] "+it.value("role","");
            if (!it.value("name","").empty()) out+=" \""+it["name"].get<std::string>()+"\"";
            else if (!it.value("desc","").empty()) out+=" \""+it["desc"].get<std::string>()+"\"";
            if (!it.value("val","").empty()) out+=" val=\""+it["val"].get<std::string>().substr(0,30)+"\"";
            if (!it.value("state","").empty()) out+=" "+it["state"].get<std::string>();
            out+="\n";
        }
        if (!text.empty()) out+="--- page text ---\n"+text+"\n";
        return out;
    }

    if (action == "find"){
        // target by text without re-scanning: filter the nodes the last observe
        // already indexed, by their accessible name / description / value.
        std::string text = args.value("text", "");
        if (text.empty()) return "error: find needs text\n";
        if (ST.selmap.empty()) return "error: nothing indexed - run observe first\n";
        std::string lo=text;
        for (auto& c:lo) c=(char)tolower((unsigned char)c);
        std::string out;
        int cnt=0;
        for (auto& it : ST.selmap){
            std::string hay=((it.value("name","")+" "+it.value("desc","")+" "+it.value("val","")));
            for (auto& c:hay) c=(char)tolower((unsigned char)c);
            if (hay.find(lo)==std::string::npos) continue;
            out+="["+std::to_string((int)it["n"])+"] "+it.value("role","")+" \""
                +it.value("name","")+"\"\n"; cnt++;
            if (cnt>=20) break;
        }
        if (!cnt) return "no indexed element matches \""+text+"\"; observe again or try a shorter phrase\n";
        return "matches for \""+text+"\" (act by [n]):\n"+out;
    }

    if (action == "click"){
        if (!args.contains("n")) return "error: click needs n\n";
        int n=args["n"].get<int>();
        double x,y; std::string tag,text,oid;
        err=resolve_n_ax(n,x,y,tag,text,oid);
        if (!err.empty()) return err;
        if (!(err=mouse_event("mouseMoved",x,y,0,0)).empty()) return err;
        if (!(err=mouse_event("mousePressed",x,y,1,1)).empty()) return err;
        if (!(err=mouse_event("mouseReleased",x,y,0,1)).empty()) return err;
        usleep(200000);
        json s; status_json(s);
        return "clicked ["+std::to_string(n)+"] "+tag+" \""+text+"\" url: "+s.value("url","")+"\n";
    }

    if (action == "click_xy"){
        double x=args.value("x",0.0), y=args.value("y",0.0);
        if (!(err=mouse_event("mouseMoved",x,y,0,0)).empty()) return err;
        if (!(err=mouse_event("mousePressed",x,y,1,1)).empty()) return err;
        if (!(err=mouse_event("mouseReleased",x,y,0,1)).empty()) return err;
        usleep(200000);
        return "clicked ("+std::to_string(x)+","+std::to_string(y)+")\n";
    }

    if (action == "type"){
        if (!args.contains("n") || !args.contains("text"))
            return "error: type needs n and text\n";
        int n=args["n"].get<int>();
        std::string text=args["text"].get<std::string>();
        double x,y; std::string tag,text2,oid;
        err=resolve_n_ax(n,x,y,tag,text2,oid);
        if (!err.empty()) return err;
        if (!(err=mouse_event("mouseMoved",x,y,0,0)).empty()) return err;
        if (!(err=mouse_event("mousePressed",x,y,1,1)).empty()) return err;
        if (!(err=mouse_event("mouseReleased",x,y,0,1)).empty()) return err;
        usleep(150000);
        clear_selection(ST.selmap[n-1].value("backend",-1ll), oid);
        json res;
        err=cdp("Input.insertText", {{"text", text}}, res, 10);
        if (!err.empty()) return err;
        return "typed into ["+std::to_string(n)+"] "+tag+"\n";
    }

    if (action == "type_text"){
        // type into whatever is focused (after a click) without an index. does
        // not clear first - clearing is type(n)'s job.
        if (!args.contains("text")) return "error: type_text needs text\n";
        std::string text=args["text"].get<std::string>();
        if (text.empty()) return "error: type_text needs text\n";
        json res;
        err=cdp("Input.insertText", {{"text", text}}, res, 10);
        if (!err.empty()) return err;
        return "inserted "+std::to_string(text.size())+" chars into the focused element\n";
    }

    if (action == "press"){
        std::string key=args.value("key","Enter");
        struct K { const char* name; const char* key_; const char* code; int vk; };
        static const K keys[]={
            {"enter","Enter","Enter",13},{"tab","Tab","Tab",9},
            {"escape","Escape","Escape",27},{"backspace","Backspace","Backspace",8},
            {"delete","Delete","Delete",46},{"arrowup","ArrowUp","ArrowUp",38},
            {"arrowdown","ArrowDown","ArrowDown",40},{"arrowleft","ArrowLeft","ArrowLeft",37},
            {"arrowright","ArrowRight","ArrowRight",39},{"space"," ","Space",32},
            {"home","Home","Home",36},{"end","End","End",35},
            {"pageup","PageUp","PageUp",33},{"pagedown","PageDown","PageDown",34}};
        std::string lo=key; for (auto& c:lo) c=(char)tolower((unsigned char)c);
        const K* k=nullptr; for (auto& kk:keys) if (lo==kk.name){ k=&kk; break; }
        if (!k) return "error: unsupported key\n";
        json res;
        json down={{"type","keyDown"},{"key",k->key_},{"code",k->code},
                   {"windowsVirtualKeyCode",k->vk},{"nativeVirtualKeyCode",k->vk}};
        if (k->vk==32) down["text"]=" ";
        if (!(err=cdp("Input.dispatchKeyEvent", down, res, 10)).empty()) return err;
        json up=down; up["type"]="keyUp"; up.erase("text");
        if (!(err=cdp("Input.dispatchKeyEvent", up, res, 10)).empty()) return err;
        return "pressed "+key+"\n";
    }

    if (action == "hover"){
        if (!args.contains("n")) return "error: hover needs n\n";
        int n=args["n"].get<int>();
        double x,y; std::string tag,text,oid;
        err=resolve_n_ax(n,x,y,tag,text,oid);
        if (!err.empty()) return err;
        if (!(err=mouse_event("mouseMoved",x-5,y-5,0,0)).empty()) return err;
        if (!(err=mouse_event("mouseMoved",x,y,0,0)).empty()) return err;
        usleep(200000);
        return "hovered ["+std::to_string(n)+"]\n";
    }

    auto do_drag=[&](double x1,double y1,double x2,double y2,int steps)->std::string{
        if (!(err=mouse_event("mousePressed",x1,y1,1,1)).empty()) return err;
        for (int i=1;i<=steps;i++){
            double x=x1+(x2-x1)*i/steps, y=y1+(y2-y1)*i/steps;
            if (!(err=mouse_event("mouseMoved",x,y,1,0)).empty()) return err;
            usleep(16000);
        }
        if (!(err=mouse_event("mouseReleased",x2,y2,0,1)).empty()) return err;
        usleep(150000); return "";
    };

    if (action == "drag"){
        if (!args.contains("from_n") || !args.contains("to_n"))
            return "error: drag needs from_n and to_n\n";
        double x1,y1,x2,y2; std::string t1,a1,o1,t2,a2,o2;
        err=resolve_n_ax(args["from_n"].get<int>(),x1,y1,t1,a1,o1);
        if (!err.empty()) return err;
        err=resolve_n_ax(args["to_n"].get<int>(),x2,y2,t2,a2,o2);
        if (!err.empty()) return err;
        err=do_drag(x1,y1,x2,y2,12);
        if (!err.empty()) return err;
        return "dragged ["+std::to_string(args["from_n"].get<int>())+"] onto ["
             +std::to_string(args["to_n"].get<int>())+"]\n";
    }

    if (action == "drag_xy"){
        if (!args.contains("x1")||!args.contains("y1")||!args.contains("x2")||!args.contains("y2"))
            return "error: drag_xy needs x1,y1,x2,y2\n";
        err=do_drag(args.value("x1",0.0),args.value("y1",0.0),
                    args.value("x2",0.0),args.value("y2",0.0),12);
        if (!err.empty()) return err;
        return "dragged -> "+std::to_string(args.value("x2",0.0))+","+std::to_string(args.value("y2",0.0))+"\n";
    }

    if (action == "scroll"){
        int dy=args.value("dy",300);
        std::string v=eval_js("window.scrollBy(0,"+std::to_string(dy)+");({y:window.scrollY,h:document.body.scrollHeight})");
        if (v.rfind("error",0)==0) return v;
        return "scrolled "+std::to_string(dy)+": "+v+"\n";
    }

    if (action == "read"){
        std::string sel=args.value("selector","");
        if (sel.empty()) return "error: read needs selector\n";
        std::string js="(function(){var e=document.querySelector("+json(sel).dump()+
            ");if(!e)return '';var t=(e.tagName==='INPUT'||e.tagName==='TEXTAREA'||"
            "e.tagName==='SELECT')?String(e.value||''):(e.innerText||'');return t.slice(0,100000);})()";
        std::string v=eval_js(js);
        if (v.rfind("error",0)==0) return v;
        try { return json::parse(v).get<std::string>()+"\n"; }
        catch (...) { return v+"\n"; }
    }

    if (action == "eval"){
        std::string js=args.value("js","");
        if (js.empty()) return "error: eval needs js\n";
        std::string v=eval_js(js,60);
        if (v.rfind("error",0)==0) return v;
        return (v=="null"?"null\n":v+"\n");
    }

    if (action == "screenshot"){
        bool full=args.value("full", true);
        json params={{"format","png"},{"captureBeyondViewport",full}};
        json res;
        err=cdp("Page.captureScreenshot", params, res, 30);
        if (!err.empty()) return err;
        std::string png;
        if (!b64_decode(res.value("data",""), png))
            return "error: screenshot decode failed\n";
        std::string path=args.value("path","");
        if (path.empty()) path="/tmp/browser-shot-"+std::to_string(time(nullptr))+".png";
        FILE* f=fopen(path.c_str(),"wb");
        if (!f) return "error: cannot write "+path+"\n";
        fwrite(png.data(),1,png.size(),f); fclose(f);
        return "screenshot saved: "+path+" ("+std::to_string(png.size())+" bytes)\n";
    }

    return "error: unknown action '"+action+
           "'. actions: status, navigate, go_back, go_forward, reload, observe, find, click, "
           "click_xy, type, type_text, press, hover, drag, drag_xy, scroll, read, eval, screenshot, "
           "restart, close\n";
}

REGISTER_TOOL_DESC("browser", tool_browser,
R"(browser(action, ...): native c++ browser automation over raw CDP - the agent drives a real headless chromium. the page is exposed as its ACCESSIBILITY TREE, so you act on semantic controls (role + name), not css selectors.
-standard flow: navigate(url), then observe, then act by [n]. observe lists every interactive, visible element one per line as [n] role "name" + state; find(text) locates the [n] for a phrase without re-reading the whole page.
-actions: status, navigate(url), go_back, go_forward, reload, observe(), find(text), click(n), click_xy(x,y), type(n,text) (focus+clear+type), type_text(text) (type into focused), press(key), hover(n), drag(from_n,to_n), drag_xy, scroll(dy), read(selector), eval(js), screenshot(path?,full?), restart, close
-indices are from the LAST observe and go stale after navigation or reload: observe again. for canvas/svg or anything without a role, use the page text + click_xy with coordinates from a screenshot.
-screenshot(path?) saves a png and attaches it as a real image (vision models) - use it to see grids/games/images. cookies persist in a profile dir; chromium lazily starts, idles out after 900s, relaunches transparently.
-for plain text search use web_search; use this tool for javascript pages, clicking, forms, and session state.)");
