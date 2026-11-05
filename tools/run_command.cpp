#include "../tool.h"
#include <string>
#include <map>
#include <mutex>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <pty.h>
#include <signal.h>
#include <termios.h>
#include <sys/wait.h>
#include <chrono>

// run_command runs shell commands. commands that prompt for input are run on
// a pseudo-terminal so they behave as if interactive; when the child goes idle
// while still alive we hand control back to the model, which replies with the
// input to send (via a follow-up call carrying session_id + input). this lets
// the model drive multi-prompt flows instead of hanging.

namespace {

constexpr int IDLE_MS = 30000;         // no-output window that means "probably waiting"
constexpr size_t MAX_OUT = 256*1024;   // cap retained output per session

struct session{
    pid_t pid=-1;
    int fd=-1;
    std::string out;
    time_t deadline=0;  // absolute time the command must finish by; 0 = none
    int timeout_s=0;    // the requested seconds, for the report
};

std::mutex g_mu;
std::map<int, session> g_sessions;
int g_next_id=1;

// append child output, capping size
void append(session& s, const char* buf, size_t n){
    s.out.append(buf, n);
    if (s.out.size()>MAX_OUT*2)
        s.out.erase(0, s.out.size()-MAX_OUT);
}

// pump returns: 0 = child eof/error (exiting), 1 = idle (prompt), 2 = deadline hit (killed).
int pump(session& s, int idle_ms){
    char buf[4096];
    for(;;){
        int wait_ms=idle_ms;
        if (s.deadline){
            long remain=(long)(s.deadline - time(nullptr))*1000;
            if (remain<=0){ kill(-s.pid, SIGKILL); return 2; }
            if (remain<wait_ms) wait_ms=(int)remain;
        }
        struct pollfd pfd{s.fd, POLLIN, 0};
        int pr=poll(&pfd, 1, wait_ms);
        if (pr<0){
            if (errno==EINTR) continue;
            return 0;
        }
        if (pr==0){
            if (s.deadline && (long)(s.deadline - time(nullptr))<=0){ kill(-s.pid, SIGKILL); return 2; }
            return 1; // quiet while alive -> prompt
        }
        if (pfd.revents & (POLLIN|POLLHUP|POLLERR)){
            ssize_t n=read(s.fd, buf, sizeof(buf));
            if (n>0){ append(s, buf, (size_t)n); continue; }
            return 0; // eof/error: child side closed
        }
    }
}

// reap and record final status; returns wait status or -1 if still running.
int try_reap(session& s){
    int st=0;
    pid_t r=waitpid(s.pid, &st, WNOHANG);
    if (r==s.pid) return st;
    return -1;
}

int exit_code_from(int st){
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128+WTERMSIG(st);
    return -1;
}

void close_session(session& s){
    if (s.fd>=0){ close(s.fd); s.fd=-1; }
}

std::string trim_tail(const std::string& s, size_t n=400){
    if (s.size()<=n) return s;
    size_t start=s.size()-n;
    // don't split a utf8/escape mid-way; best effort
    return s.substr(start);
}

// build the message returned to the model for a paused session
std::string paused_message(int id, const std::string& tail){
    std::string r;
    r+="command is waiting for input (session_id: "+std::to_string(id)+")\n";
    r+="output so far:\n"+tail;
    if (!tail.empty() && tail.back()!='\n') r+="\n";
    r+="---\n";
    r+="if it is prompting, reply by calling run_command again with {\"session_id\": "+std::to_string(id)+", \"input\": \"<text to send>\"}. ";
    r+="the input is sent as one line. to abandon it, call run_command with {\"session_id\": "+std::to_string(id)+", \"cancel\": true}. ";
    r+="if you believe it is merely slow rather than prompting, you may send empty input to keep waiting.";
    return r;
}

// drain any remaining buffered bytes (non-blocking)
void drain(session& s){
    char buf[4096];
    for(;;){
        ssize_t n=read(s.fd, buf, sizeof(buf));
        if (n>0){ append(s, buf, (size_t)n); continue; }
        break;
    }
}

std::string finish_message(int st, const std::string& out){
    return "exit code: "+std::to_string(exit_code_from(st))+"\nresult: "+out+'\n';
}

// run until the child exits or goes idle. sets finished=true when the child
// is done (caller drops the session); finished=false means it is paused at a
// prompt and the caller keeps the session.
void drive(session& s, std::string& result, bool& finished){
    int p=pump(s, IDLE_MS);
    if (p==2){
        // deadline: the whole group was killed in pump; reap and report.
        int st=0;
        while (waitpid(s.pid, &st, 0)<0 && errno==EINTR) {}
        drain(s);
        result="timeout after "+std::to_string(s.timeout_s)+"s; output so far:\n"+s.out+"\n";
        finished=true;
        return;
    }
    if (p==1){
        // alive but quiet. it may be prompting, or exiting right now (the pty
        // can go quiet a beat before EOF). short grace to catch imminent exit
        // or trailing output before we commit to "paused".
        for (int i=0; i<8; i++){
            int st0=try_reap(s);
            if (st0!=-1){ drain(s); result=finish_message(st0, s.out); finished=true; return; }
            int still=pump(s, 50);
            if (still!=1) break; // eof, or deadline (handled below)
        }
        p=pump(s, IDLE_MS);
        if (p==2){
            int st=0;
            while (waitpid(s.pid, &st, 0)<0 && errno==EINTR) {}
            drain(s);
            result="timeout after "+std::to_string(s.timeout_s)+"s; output so far:\n"+s.out+"\n";
            finished=true;
            return;
        }
    }
    if (p==0){
        // pty closed: the child is exiting/exited. blocking reap is correct
        // here (WNOHANG can miss the status for a moment after EOF).
        int st=0;
        while (waitpid(s.pid, &st, 0)<0 && errno==EINTR) {}
        drain(s);
        result=finish_message(st, s.out);
        finished=true;
        return;
    }
    // idle and still alive: paused at a prompt.
    finished=false;
}

// write the whole buffer to the pty master, handling partial writes,
// EAGAIN (master is nonblocking), and EINTR. returns false on hard error.
bool write_all(int fd, const std::string& data){
    size_t off=0;
    while (off<data.size()){
        ssize_t w=write(fd, data.data()+off, data.size()-off);
        if (w>0){ off+=(size_t)w; continue; }
        if (w<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
            struct pollfd pfd{fd, POLLOUT, 0};
            int pr;
            do { pr=poll(&pfd, 1, 1000); } while (pr<0 && errno==EINTR);
            if (pr<0) return false;
            continue; // retry; timeout just loops and checks again
        }
        if (w<0 && errno==EINTR) continue;
        return false; // EIO/EPIPE/etc: child side gone
    }
    return true;
}

// read a session_id argument as int without throwing on bad types.
bool get_sid(const nlohmann::json& args, int& out){
    if (!args.is_object() || !args.contains("session_id")) return false;
    const auto& v=args["session_id"];
    if (v.is_number_integer()){ out=v.get<long long>(); return true; }
    if (v.is_number()){ out=(int)v.get<double>(); return true; }
    if (v.is_string()){ // lenient: models sometimes send ids as text
        try{ out=std::stoi(v.get<std::string>()); return true; }
        catch(...){ return false; }
    }
    return false; // bools, objects, etc: not a valid id
}

// reap and drop any sessions whose child already exited. prevents zombie
// accumulation when a paused command dies on its own and the model never
// continues or cancels it.
void sweep_dead(){
    for (auto it=g_sessions.begin(); it!=g_sessions.end(); ){
        session& s=it->second;
        int st=0;
        pid_t r=waitpid(s.pid, &st, WNOHANG);
        if (r==s.pid){
            close_session(s);
            it=g_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

static std::string tool_run_command(const nlohmann::json& args, tool_context&){
    if (!args.is_object()) return "error: run_command expects an object argument\n";
    std::lock_guard<std::mutex> lk(g_mu);
    sweep_dead();

    // --- cancel path ---
    bool cancel=args.value("cancel", false);
    if (args.contains("session_id") && cancel){
        int id=0;
        if (!get_sid(args, id)) return "error: session_id must be an integer\n";
        auto it=g_sessions.find(id);
        if (it==g_sessions.end()) return "error: no such session "+std::to_string(id)+"\n";
        session& s=it->second;
        if (s.pid>0){
            kill(-s.pid, SIGKILL);  // whole group: grandchildren included
            int st=0;
            while (waitpid(s.pid, &st, 0)<0 && errno==EINTR) {}
        }
        close_session(s);
        std::string r="session "+std::to_string(id)+" cancelled\n";
        g_sessions.erase(it);
        return r;
    }

    // --- continue existing session ---
    if (args.contains("session_id")){
        int id=0;
        if (!get_sid(args, id)) return "error: session_id must be an integer\n";
        auto it=g_sessions.find(id);
        if (it==g_sessions.end()) return "error: no such session "+std::to_string(id)+"\n";
        session& s=it->second;
        std::string in=args.value("input", std::string(""));
        // empty input means "keep waiting": poll again WITHOUT writing
        // anything, so we never inject a stray newline into a process that
        // was only slow rather than prompting.
        if (!in.empty()){
            std::string line=in+"\n";
            if (!write_all(s.fd, line))
                return std::string("error: write to session failed (child may have exited): ")+strerror(errno)+"\n";
        }
        std::string result; bool finished=false;
        drive(s, result, finished);
        if (finished){ close_session(s); g_sessions.erase(it); return result; }
        return paused_message(id, trim_tail(s.out));
    }

    // --- new command ---
    if (!args.contains("command"))
        return "error: missing 'command' argument (or provide session_id to continue a session)\n";
    std::string rcommand=args["command"].get<std::string>();
    int timeout_s=0;
    if (args.contains("timeout") && args["timeout"].is_number())
        timeout_s=args["timeout"].get<int>();
    if (timeout_s<1) timeout_s=0;
    if (timeout_s>3600) timeout_s=3600;

    // the pty merges stderr into the terminal; the raw command goes to bash -lc

    // build a deterministic terminal mode. do NOT derive it from the parent's
    // stdin: if stdin is a pipe/closed, tcgetattr fails and leaves VMIN=0,
    // which makes the child's read() return EOF instantly instead of blocking.
    struct termios ios;
    memset(&ios, 0, sizeof(ios));
    cfmakeraw(&ios);        // no echo, no cr translation, raw input
    ios.c_cc[VMIN]=1;       // block until at least one byte -> real prompts wait
    ios.c_cc[VTIME]=0;

    int master=-1;
    char namebuf[128];
    pid_t pid=forkpty(&master, namebuf, &ios, NULL);
    if (pid<0) return std::string("forkpty failed: ")+strerror(errno)+"\n";
    // own process group so cancel can kill the whole subtree, not just the
    // bash: a 'bash -lc "sleep 300"' cancelled must not leave the sleep alive.
    // note: forkpty already setsid()s the child (new session + group leader),
    // so the subtree shares one group with no extra call. an explicit parent
    // setpgid here is not just redundant - empirically it breaks command
    // execution (child exits 1 before running anything).
    if (pid==0){
        // die with the parent: if the agent process exits or crashes, no
        // orphaned command trees. the pty's hangup then hups the subtree.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        execl("/bin/bash", "bash", "-lc", rcommand.c_str(), (char*)nullptr);
        _exit(127);
    }
    // parent
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);
    int id=g_next_id++;
    session s; s.pid=pid; s.fd=master;
    s.timeout_s=timeout_s;
    if (timeout_s) s.deadline=time(nullptr)+timeout_s;

    std::string result; bool finished=false;
    drive(s, result, finished);
    if (finished){ close_session(s); return result; }
    g_sessions[id]=std::move(s);
    return paused_message(id, trim_tail(g_sessions[id].out));
}

REGISTER_TOOL_DESC("run_command", tool_run_command,
R"(run_command(command, timeout?):
-runs a command in my ubuntu 24.04 sandbox; optional timeout (1-3600s) bounds total runtime and kills the whole process group on expiry
-if the command goes quiet for 30s while still running, returns with a session_id; reply by calling run_command again with {"session_id": N, "input": "text"} or {"session_id": N, "cancel": true}; empty input keeps waiting)");
