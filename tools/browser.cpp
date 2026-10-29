#include "../tool.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>

// one browser action per call: connects to the browser daemon's unix
// socket, sends one json request, reads one json response. the daemon is
// a separate long-lived process (~/browserd.py) so the page state and
// cookies persist across calls; this tool is just the thin pipe.
static std::string tool_browser(const nlohmann::json& args, tool_context&){
    if (!args.contains("action"))
        return "error: missing 'action' argument\n";
    std::string action=args["action"].get<std::string>();

    nlohmann::json req;
    req["action"]=action;
    if (args.contains("url") && args["url"].is_string())
        req["url"]=args["url"];
    if (args.contains("selector") && args["selector"].is_string())
        req["selector"]=args["selector"];
    if (args.contains("text") && args["text"].is_string())
        req["text"]=args["text"];
    if (args.contains("js") && args["js"].is_string())
        req["js"]=args["js"];
    if (args.contains("limit") && args["limit"].is_number())
        req["limit"]=args["limit"];
    if (args.contains("timeout") && args["timeout"].is_number())
        req["timeout"]=args["timeout"];
    if (args.contains("path") && args["path"].is_string())
        req["path"]=args["path"];
    if (args.contains("full") && args["full"].is_boolean())
        req["full"]=args["full"];

    std::string payload=req.dump()+"\n";
    if (payload.size()>512*1024)
        return "error: request exceeds 512KB (js payloads too large)\n";

    int fd=socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd<0) return "error: socket() failed\n";
    struct sockaddr_un addr{};
    addr.sun_family=AF_UNIX;
    std::string sockpath="/tmp/browserd.sock";
    if (strlen(addr.sun_path)+sockpath.size()+1>sizeof(addr.sun_path))
        return "error: socket path too long\n";
    strncpy(addr.sun_path, sockpath.c_str(), sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr))!=0){
        close(fd);
        return "error: browser daemon not reachable at "+sockpath+
               " (start it: setsid nohup ~/browserd.py > /tmp/browserd.log 2>&1 &)\n";
    }

    std::string resp;
    // send
    {
        const char* p=payload.data();
        size_t left=payload.size();
        while (left>0){
            ssize_t n=send(fd, p, left, 0);
            if (n<=0){ close(fd); return "error: send failed\n"; }
            p+=n; left-=(size_t)n;
        }
    }
    // read one line
    {
        char buf[65536];
        bool done=false;
        while (!done){
            ssize_t n=recv(fd, buf, sizeof(buf), 0);
            if (n<0){ close(fd); return "error: recv failed\n"; }
            if (n==0) break;
            for (ssize_t i=0;i<n;i++){
                if (buf[i]=='\n'){ done=true; break; }
                resp+=buf[i];
            }
            if (resp.size()>8*1024*1024){
                close(fd); return "error: response exceeds 8MB\n";
            }
        }
    }
    close(fd);
    if (resp.empty())
        return "error: daemon closed connection without a response (it may have crashed - check /tmp/browserd.log)\n";
    // pretty-print the daemon's json response
    try {
        nlohmann::json r=nlohmann::json::parse(resp);
        if (r.value("ok", false)){
            std::string out;
            for (auto& [k,v] : r.items()){
                if (k=="ok") continue;
                out+=k+": "+v.dump()+"\n";
            }
            return out.empty()?"ok\n":out;
        } else {
            return "error (daemon): "+r.value("error", "unknown")+"\n";
        }
    } catch (...) {
        return resp+"\n";
    }
}

REGISTER_TOOL_DESC("browser", tool_browser,
R"(browser(action, url?, selector?, text?, js?, limit?, timeout?, path?, full?):
-controls a long-lived headless chromium through the local browser daemon (/tmp/browserd.sock, daemon: ~/browserd.py, launch: setsid nohup ~/browserd.py > /tmp/browserd.log 2>&1 &)
-actions: status, navigate(url), extract(selector?), links(limit?), click(selector), type(selector,text), eval(js), html, screenshot(path?,full?), back, forward, reset
-one page, shared across calls; cookies persist via the profile dir; if the daemon is down the tool says how to start it
-each call is one connection; the daemon exits after 600s idle - if it died, restart it and retry the failed action
-for plain text search needs prefer web_search; use the browser for javascript-rendered pages, clicks, and session-sticky sites)");
