#include "tool.h"
#include <map>
#include <mutex>

static std::map<std::string, tool>& registry(){
    static std::map<std::string, tool> r;
    return r;
}
static std::vector<std::string>& order(){
    static std::vector<std::string> o;
    return o;
}

void register_tool(tool t){
    auto& r=registry();
    if (r.find(t.name)!=r.end()){
        // last registration wins; keeps behavior predictable if two files
        // declare the same name.
        r[t.name]=std::move(t);
        return;
    }
    order().push_back(t.name);
    r[t.name]=std::move(t);
}

std::string render_tools(){
    // concatenates every tool desc into the system-prompt tools section.
    std::string out;
    for (const std::string& n: order()){
        const tool& t=registry()[n];
        out+=t.desc;
        if (out.empty() || out.back()!=10) out+=std::string(1, (char)10);
        out+=std::string(1, (char)10);
    }
    if (out.size()>=2 && out.compare(out.size()-2, 2, std::string(2, (char)10))==0)
        out.resize(out.size()-1);
    return out;
}

const tool* find_tool(const std::string& name){
    auto& r=registry();
    auto it=r.find(name);
    return it==r.end() ? nullptr : &it->second;
}

std::vector<std::string> tool_names(){
    return order();
}
