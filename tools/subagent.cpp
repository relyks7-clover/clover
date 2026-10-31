#include "../tool.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>

// subagent tool: runs a fresh copy of clover (own executable, -t mode,
// scratch context + scratch memory dir) framed by a purpose. the copy has
// every tool, so recursion is native; depth is capped via SUBAGENT_DEPTH.
// the provider allows one request at a time, so this tool blocks until the
// child exits: batched calls in one array still run strictly sequentially.
static std::string tool_subagent(const nlohmann::json& args, tool_context&){
    if (!args.contains("task"))
        return "error: missing 'task' argument\n";
    std::string task=args["task"].get<std::string>();
    if (task.empty()) return "error: 'task' must not be empty\n";
    if (args.contains("purpose") && !args["purpose"].is_string())
        return "error: 'purpose' must be a string\n";
    std::string purpose=args.value("purpose", "carry out the task");
    int max_seconds=600;
    if (args.contains("timeout") && args["timeout"].is_number())
        max_seconds=args["timeout"].get<int>();
    if (max_seconds<10) max_seconds=10;
    if (max_seconds>3600) max_seconds=3600;
    long long depth=0;
    { const char* d=getenv("SUBAGENT_DEPTH"); if (d) depth=atoll(d); }
    if (depth>=3)
        return "error: subagent depth cap reached ("+std::to_string(depth)+"); do the work yourself\n";

    // resolve own exe: the copy must run the same binary even if cwd moved
    char exepath[4096]={};
    ssize_t n=readlink("/proc/self/exe", exepath, sizeof(exepath)-1);
    if (n<=0) return "error: cannot resolve own executable\n";
    std::string exe(exepath, (size_t)n);

    // scratch dir: isolated context and memory for the child
    char tpl[]="/tmp/subagent-XXXXXX";
    char* dir=mkdtemp(tpl);
    if (!dir) return "error: mkdtemp failed\n";
    std::string sdir(dir);
    std::string ctx=sdir+"/ctx.json";

    // the task is the child's stdin: -t mode reads it to exhaustion.
    // the parent's own stdin is already drained, so it must NOT be inherited.
    std::string taskfile=sdir+"/task";
    {
        std::ofstream tf(taskfile, std::ios::binary|std::ios::trunc);
        if (!tf.is_open()) return "error: cannot write task file\n";
        tf<<task;
        tf.flush();
        if (!tf.good()) return "error: write failed on task file\n";
    }
    int infd=open(taskfile.c_str(), O_RDONLY);

    int outfd=open((sdir+"/out").c_str(), O_CREAT|O_RDWR, 0600);
    int errfd=open((sdir+"/err").c_str(), O_CREAT|O_RDWR, 0600);
    if (outfd<0 || errfd<0) return "error: cannot open capture files\n";

    pid_t pid=fork();
    if (pid<0) return "error: fork failed\n";
    // double setpgid (parent here, child below): whichever lands first
    // wins; closes the race where kill(-pid) fires before the child
    // grouped itself and orphans the subtree.
    setpgid(pid, pid);
    if (pid==0){
        // child: its own process group so a timeout kill reaps the subtree
        setpgid(0,0);
        dup2(infd, STDIN_FILENO);
        dup2(outfd, STDOUT_FILENO);
        dup2(errfd, STDERR_FILENO);
        setenv("SUBAGENT_DEPTH", std::to_string(depth+1).c_str(), 1);
        setenv("MEMORY_DIR", (sdir+"/mem").c_str(), 1);
        mkdir((sdir+"/mem").c_str(), 0700);
        execl(exe.c_str(), exe.c_str(), "-t", "--ctx", ctx.c_str(),
              "--purpose", purpose.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(outfd); close(errfd); close(infd);

    bool timed_out=false;
    long long ticks=0;
    for (;;){
        int status; pid_t r=waitpid(pid, &status, WNOHANG);
        if (r==pid) break;
        if (r<0){ kill(-pid, SIGKILL); return "error: waitpid failed\n"; }
        if (ticks>=max_seconds*5){
            kill(-pid, SIGKILL);  // negative pid: whole process group
            waitpid(pid, &status, 0);
            timed_out=true;
            break;
        }
        usleep(200000);
        ticks++;
        if (ticks%25==0) std::cerr<<"[subagent still running, "<<(ticks/5)<<"s]\n";
    }

    std::ostringstream oss; oss<<std::ifstream(sdir+"/out").rdbuf();
    std::string body=oss.str();
    size_t nl=body.find('\n');
    if (nl!=std::string::npos && body.rfind("[", 0)==0) body=body.substr(nl+1);
    if (timed_out){
        body="subagent timed out after "+std::to_string(max_seconds)+
             "s; partial output follows.\n"+body;
        // the child's stderr says why it stalled (retries, tool errors):
        // tail it into the report so the caller can act, capped hard.
        std::ostringstream es; es<<std::ifstream(sdir+"/err").rdbuf();
        std::string errtail=es.str();
        if (errtail.size()>1200) errtail="..."+errtail.substr(errtail.size()-1200);
        if (!errtail.empty()) body+="\n--- child stderr tail ---\n"+errtail;
    }
    if (body.empty()) body="(subagent produced no output)\n";
    std::string rm="rm -rf "+sdir;
    int rc=system(rm.c_str());
    (void)rc;
    return body;
}

REGISTER_TOOL_DESC("subagent", tool_subagent,
R"(subagent(task, purpose?, timeout?):
-runs a fresh copy of clover on the task and returns its final report; the copy shares no context or memory with me
-purpose (optional) frames the copy: one sentence on its role; timeout (optional, default 600s, range 10-3600) bounds the subtree
-recursion is native (subagents may spawn subagents, depth-capped); scheduling is strict-sequential: the provider allows one request at a time, so calls run one after another and a batch adds wall-clock time, never parallelism
-use for self-contained, well-scoped work; write the task so a stranger could complete it; their report is a claim - re-verify anything critical myself)");
