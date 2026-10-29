#include "../tool.h"
#include <string>
#include <fstream>
#include <vector>

// reads a file for the agent. deliberately small: whole file or a line
// window, binary-safe, with a size cap so a huge file cannot flood context.
static std::string tool_read_file(const nlohmann::json& args, tool_context&){
    if (!args.contains("path"))
        return "error: missing 'path' argument\n";
    std::string path=args["path"].get<std::string>();
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return "error: cannot open "+path+"\n";
    f.seekg(0, std::ios::end);
    long long sz=f.tellg();
    if (sz<0) return "error: cannot stat "+path+"\n";
    if (sz>10*1024*1024) return "error: file too large (>10MB): "+path+"\n";
    f.seekg(0, std::ios::beg);
    std::vector<char> buf((size_t)sz);
    f.read(buf.data(), buf.size());
    if (!f.good() && !f.eof()) return "error: read failed on "+path+"\n";
    std::string data(buf.data(), (size_t)f.gcount());
    long long lo=1, hi=0; // 1-based, inclusive; 0 = open-ended
    if (args.contains("lo") && args["lo"].is_number()) lo=args["lo"].get<long long>();
    if (args.contains("hi") && args["hi"].is_number()) hi=args["hi"].get<long long>();
    if (lo<1) lo=1;
    if (lo>hi && hi>0) return "error: lo>hi\n";
    // slice lines [lo, hi]
    std::string out;
    long long line=1;
    size_t pos=0;
    while (pos<data.size() || (pos==0 && data.empty())){
        size_t nl=data.find('\n', pos);
        std::string l=data.substr(pos, nl==std::string::npos?std::string::npos:nl-pos);
        if (line>=lo && (hi==0 || line<=hi)){
            out+=std::to_string(line)+": "+l+"\n";
            if (out.size()>512*1024) return "error: requested range exceeds 512KB output cap; narrow lo/hi\n";
        }
        line++;
        if (nl==std::string::npos) break;
        pos=nl+1;
        if (line>2000000) break; // pathological no-newline guard
    }
    if (out.empty()) return "(empty range or empty file)\n";
    return out;
}

REGISTER_TOOL_DESC("read_file", tool_read_file,
R"(read_file(path, lo?, hi?):
-reads a file and returns it with 1-based line numbers; lo/hi select a line window (both optional, hi=0 means open-ended)
-binary-safe; files >10MB are rejected, output is capped at 512KB - use lo/hi windows for large files
-use this instead of shell cat for inspecting files: no quoting hazards, no pty mangling)");