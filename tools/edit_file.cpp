#include "../tool.h"
#include <sys/stat.h>
#include <string>
#include <fstream>

// surgical text edit: find a unique anchor in the file, replace it. the
// anchor must match exactly once - ambiguity is an error, never a guess.
static std::string tool_edit_file(const nlohmann::json& args, tool_context&){
    for (const char* k: {"path", "old", "new"})
        if (!args.contains(k))
            return std::string("error: missing '")+k+"' argument\n";
    std::string path=args["path"].get<std::string>();
    std::string old_s=args["old"].get<std::string>();
    std::string new_s=args["new"].get<std::string>();
    if (old_s.empty()) return "error: 'old' must not be empty\n";
    struct stat stg;
    if (stat(path.c_str(), &stg)!=0) return "error: cannot stat "+path+"\n";
    if (!S_ISREG(stg.st_mode)) return "error: not a regular file: "+path+"\n";
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "error: cannot open "+path+"\n";
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t first=data.find(old_s);
    if (first==std::string::npos)
        return "error: anchor not found in "+path+"\n";
    if (data.find(old_s, first+1)!=std::string::npos)
        return "error: anchor is not unique (matches more than once) - extend the anchor\n";
    // preserve the existing file mode across the tmp+rename, like save()
    mode_t keep=0600;
    struct stat st;
    if (stat(path.c_str(), &st)==0) keep=st.st_mode & 07777;
    std::string tmp=path+".edit.tmp";
    {
        std::ofstream out(tmp, std::ios::binary|std::ios::trunc);
        if (!out.is_open()) return "error: cannot open "+tmp+" for writing\n";
        out<<data.substr(0, first)<<new_s<<data.substr(first+old_s.size());
        out.flush();
        if (!out.good()) return "error: write failed to "+tmp+"\n";
    }
    if (chmod(tmp.c_str(), keep)!=0) return "error: chmod failed on "+tmp+"\n";
    if (rename(tmp.c_str(), path.c_str())!=0) return "error: rename failed\n";
    return "edited "+path+": replaced 1 occurrence ("+std::to_string(old_s.size())+" -> "+std::to_string(new_s.size())+" bytes)\n";
}

REGISTER_TOOL_DESC("edit_file", tool_edit_file,
R"(edit_file(path, old, new):
-surgical edit: 'old' must appear exactly once in the file; it is replaced by 'new'
-anchor not found and non-unique anchor are both errors - the tool never guesses; regular files only (fifos/sockets rejected)
-preserves the file's mode; write is tmp+rename, atomic on posix
-for new files use write via shell, or edit an existing template; binary files are not supported)");