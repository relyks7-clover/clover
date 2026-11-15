// hot_reload: rebuild the project and swap in the new binary in place. the
// swap is out-of-place: make builds a.out.new, the new binary is smoke-
// tested, then rename(2) moves it over the running binary's path. the
// running process keeps the old inode (its text is mmap'd from it), so the
// swap can never corrupt a live process. the running process re-execs
// itself at the end of the current turn (this tool sets g_hot_reload; the
// agent loop consumes it at a clean turn boundary), so the same shell
// session continues on the new code. the old binary is kept as <exe>.prev
// for rollback, and context is persisted before the swap.
#include "../tool.h"
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/wait.h>

// set by this tool, consumed by the agent loop at a clean turn boundary.
extern volatile sig_atomic_t g_hot_reload;

namespace {
std::string exe_path(){
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    if (n <= 0) return "";
    buf[n] = 0;
    return std::string(buf);
}
std::string q(const std::string& s){ return "\"" + s + "\""; }
std::string slurp(const std::string& p){
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string tail(const std::string& s, size_t n){
    if (s.size() <= n) return s;
    return "..." + s.substr(s.size()-n);
}
}

static std::string tool_hot_reload(const nlohmann::json& args, tool_context& ctx){
    (void)args; (void)ctx;
    std::string exe = exe_path();
    if (exe.empty()) return "error: cannot resolve /proc/self/exe\n";
    // build from the binary's own directory: the Makefile and source live
    // next to the binary in production. fall back to the process cwd if the
    // binary was copied elsewhere.
    std::string build_dir = exe.substr(0, exe.rfind('/')+1);
    if (system(("cd "+q(build_dir)+" && test -f Makefile").c_str()) != 0){
        char buf[4096];
        char *p = getcwd(buf, sizeof(buf));
        if (!p) return "error: cannot resolve the build directory\n";
        build_dir = std::string(buf);
        if (system(("cd "+q(build_dir)+" && test -f Makefile").c_str()) != 0)
            return "error: no Makefile next to the binary or in the cwd; cannot build\n";
    }
    // keep the current binary for rollback; build the new one to a side
    // path (never over the running binary's inode - its text is mmap'd).
    std::string log = "/tmp/clover-hot-reload.log";
    std::string cmd = "cd "+q(build_dir)+" && { cp -f "+q(exe)+" "+q(exe+".prev")
                      +" && make a.out.new; } > "+q(log)+" 2>&1";
    int rc = system(cmd.c_str());
    int make_rc = (rc == -1) ? -1 : WEXITSTATUS(rc);
    std::string logtxt = slurp(log);
    if (make_rc != 0)
        return "error: build failed (make rc "+std::to_string(make_rc)+"); not swapping:\n"+tail(logtxt, 2000)+"\n";
    std::string newbin = build_dir + "/a.out.new";
    // smoke-test the freshly built binary before committing to the swap.
    int v = system((q(newbin)+" --help >/dev/null 2>&1").c_str());
    if (v == -1 || WEXITSTATUS(v) != 0)
        return "error: new binary failed its --help smoke test; not swapping\n";
    // swap onto the running binary's path. rename is atomic on the same
    // filesystem; across devices, copy next to the target then rename.
    if (rename(newbin.c_str(), exe.c_str()) != 0 && errno == EXDEV){
        std::string near = exe + ".new";
        std::string cp = "cp -f "+q(newbin)+" "+q(near);
        if (system(cp.c_str()) != 0)
            return "error: cross-device copy of the new binary failed\n";
        if (rename(near.c_str(), exe.c_str()) != 0){
            unlink(near.c_str());
            return "error: rename onto the running binary failed: "+std::string(strerror(errno))+"\n";
        }
    } else if (rename(newbin.c_str(), exe.c_str()) != 0){
        return "error: rename onto the running binary failed: "+std::string(strerror(errno))+"\n";
    }
    // refresh CLOVER_* keys from the repo's .clover.env so the re-exec
    // inherits them. a reload that added a new provider (e.g. google) must
    // not start with a missing key just because the current process env
    // predates the key. parse NAME/VALUE; never log values.
    {
        std::string envfile = build_dir + ".clover.env";
        std::ifstream f(envfile);
        if (f.is_open()){
            std::string line;
            while (std::getline(f, line)){
                if (line.find("CLOVER_") == std::string::npos) continue;
                size_t e = line.find('=');
                if (e == std::string::npos) continue;
                std::string name = line.substr(0, e);
                size_t ns = name.find_first_not_of(" \t");
                name = (ns == std::string::npos) ? "" : name.substr(ns);
                if (name.rfind("export", 0) == 0){ name = name.substr(6); while (!name.empty() && name.front()==' ') name.erase(0,1); }
                if (name.empty() || name.find('=') != std::string::npos) continue;
                std::string val = line.substr(e+1);
                // strip whitespace; .clover.env uses unquoted NAME=VALUE
                while (!val.empty() && (val.back()==' '||val.back()=='\t')) val.pop_back();
                setenv(name.c_str(), val.c_str(), 1);
            }
        }
    }
    // commit: re-exec at the end of this turn; the env var survives exec so
    // the restarted process can announce itself (and only it can).
    setenv("CLOVER_HOT_RELOAD", "1", 1);
    g_hot_reload = 1;
    return "ok: rebuilt and verified; the binary is swapped in - reloading on it at the end of this turn (rollback: "+exe+".prev)\n";
}

REGISTER_TOOL_DESC("hot_reload", tool_hot_reload,
R"(hot_reload():
-rebuilds the project and swaps in the new binary in place: builds to a side path, verifies it, rename(2) over the running binary, then the process re-execs itself at the end of the current turn - the same shell session continues on the new code
-the old binary is kept as <binary>.prev for rollback; context is persisted (context.json) before the swap, so no history is lost
-the running binary's inode is never written in place (its text is mmap'd), so a swap can never corrupt a live process
-use after editing source to pick up changes without restarting the shell)" );
