#include "../tool.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string>

// appends content to a file, creating it (0600) if missing. plain in-place
// append: never rewrites the whole file, so chunked writes stay cheap.
static std::string tool_append_file(const nlohmann::json& args, tool_context&){
    for (const char* k: {"path", "content"})
        if (!args.contains(k))
            return std::string("error: missing '")+k+"' argument\n";
    std::string path=args["path"].get<std::string>();
    std::string content=args["content"].get<std::string>();
    if (content.empty()) return "error: 'content' must not be empty\n";
    // regular files only: opening a fifo for append would hang the agent
    struct stat st;
    if (stat(path.c_str(), &st)==0 && !S_ISREG(st.st_mode))
        return "error: not a regular file: "+path+"\n";
    // O_CREAT with explicit 0600: a new file must never pick up the
    // umask default (ofstream would give 0644+).
    int fd=open(path.c_str(), O_WRONLY|O_APPEND|O_CREAT, 0600);
    if (fd<0) return "error: cannot open "+path+" for appending\n";
    size_t off=0;
    while (off<content.size()){
        ssize_t w=write(fd, content.data()+off, content.size()-off);
        if (w<0){ close(fd); return "error: append failed on "+path+"\n"; }
        off+=(size_t)w;
    }
    close(fd);

    return "appended "+std::to_string(content.size())+" bytes to "+path+"\n";
}

REGISTER_TOOL_DESC("append_file", tool_append_file,
R"(append_file(path, content):
-appends content to a file, creating it at 0600 if missing; binary-safe, no size cap
-cheap in-place append (never rewrites the file): use for chunks, logs, incremental writes
-regular files only (fifos/sockets rejected); empty content is rejected, the parent directory must exist
-no lo/hi or anchors: read_file to inspect, edit_file to change existing text)");
