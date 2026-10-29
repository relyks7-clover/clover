#include "../tool.h"
#include <sys/stat.h>
#include <string>
#include <fstream>

// creates or overwrites a file with exact content. atomic tmp+rename,
// mode-preserving like save() and edit_file.
static std::string tool_write_file(const nlohmann::json& args, tool_context&){
    for (const char* k: {"path", "content"})
        if (!args.contains(k))
            return std::string("error: missing '")+k+"' argument\n";
    std::string path=args["path"].get<std::string>();
    std::string content=args["content"].get<std::string>();
    if (content.size()>10*1024*1024)
        return "error: content exceeds 10MB cap; write chunks with append_file\n";
    // mode policy: existing file keeps its mode; new file 0600 unless
    // 'mode' given (octal int like 420 for 0644).
    mode_t keep=0600;
    bool have_existing=false;
    struct stat st;
    if (stat(path.c_str(), &st)==0){ keep=st.st_mode & 07777; have_existing=true; }
    if (args.contains("mode") && args["mode"].is_number())
        keep=args["mode"].get<int>() & 07777;
    if (have_existing && args.value("exclusive", false))
        return "error: file exists: "+path+" (exclusive set)\n";
    std::string tmp=path+".wr.tmp";
    {
        std::ofstream f(tmp, std::ios::binary|std::ios::trunc);
        if (!f.is_open()) return "error: cannot open "+tmp+" for writing\n";
        f<<content;
        f.flush();
        if (!f.good()) return "error: write failed to "+tmp+"\n";
    }
    if (chmod(tmp.c_str(), keep)!=0) return "error: chmod failed on "+tmp+"\n";
    if (rename(tmp.c_str(), path.c_str())!=0) return "error: rename failed\n";
    return "wrote "+std::to_string(content.size())+" bytes to "+path+"\n";
}

REGISTER_TOOL_DESC("write_file", tool_write_file,
R"(write_file(path, content, mode?, exclusive?):
-creates or overwrites a file with exact content; binary-safe, 10MB cap
-mode (optional octal int, e.g. 420 for 0644) only applies to new files; an existing file keeps its mode
-exclusive (optional bool): refuse to overwrite an existing file
-atomic tmp+rename write; for chunks use append_file, for a new big file write the first chunk then append)");
