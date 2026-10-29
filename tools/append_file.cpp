#include "../tool.h"
#include <sys/stat.h>
#include <string>
#include <fstream>

// appends content to a file, creating it (0600) if missing. plain in-place
// append: never rewrites the whole file, so chunked writes stay cheap.
static std::string tool_append_file(const nlohmann::json& args, tool_context&){
    for (const char* k: {"path", "content"})
        if (!args.contains(k))
            return std::string("error: missing '")+k+"' argument\n";
    std::string path=args["path"].get<std::string>();
    std::string content=args["content"].get<std::string>();
    if (content.empty()) return "error: 'content' must not be empty\n";
    std::ofstream f(path, std::ios::binary|std::ios::app);
    if (!f.is_open()) return "error: cannot open "+path+" for appending\n";
    f<<content;
    f.flush();
    if (!f.good()) return "error: append failed on "+path+"\n";
    return "appended "+std::to_string(content.size())+" bytes to "+path+"\n";
}

REGISTER_TOOL_DESC("append_file", tool_append_file,
R"(append_file(path, content):
-appends content to a file, creating it at 0600 if missing; binary-safe, no size cap
-cheap in-place append (never rewrites the file): use for chunks, logs, incremental writes
-no lo/hi or anchors: read_file to inspect, edit_file to change existing text)");
