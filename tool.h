#pragma once
#include <string>
#include <vector>
#include <functional>
#include <csignal>
#include "json.hpp"

// host-level interrupt: the tty SIGINT handler sets clover_cancel; long
// operations (tool pumps, cdp waits, provider streams, backoff sleeps) poll
// it so ctrl-c aborts the current operation instead of needing a kill.
extern volatile sig_atomic_t clover_cancel;
// set by the hot_reload tool: the agent loop re-execs the process at a
// clean turn boundary so the shell session continues on the new binary.
extern volatile sig_atomic_t g_hot_reload;

// strict dump throws type_error.316 on invalid UTF-8 in string values; page,
// shell, and provider bytes are arbitrary, so dump with replacement instead.
static inline std::string jdump(const nlohmann::json& j){
    try{ return j.dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace); }
    catch(...){ return "[unserializable]"; }
}

// replaces invalid UTF-8 sequences with U+FFFD. json string values must be
// valid UTF-8 and the sources of our strings (model args, shells, pages)
// are not guaranteed to be; sanitizing at the boundary means no json
// operation on the data can ever throw a type_error.
static inline std::string jsanitize(std::string s){
    const char RPL[3] = {0xEF, 0xBF, 0xBD};
    std::string o; o.reserve(s.size());
    size_t i = 0, n = s.size();
    auto cont = [&](size_t k) { // k continuation bytes at i+1..i+k
        for (size_t j = 1; j <= k; j++)
            if (i + j >= n || (unsigned char)s[i+j] < 0x80 || (unsigned char)s[i+j] > 0xBF) return false;
        return true;
    };
    while (i < n){
        unsigned char c = (unsigned char)s[i];
        bool ok = false;
        if (c < 0x80) ok = true;
        else if (c >= 0xC2 && c <= 0xDF) ok = cont(1);
        else if (c == 0xE0) ok = cont(1) && (unsigned char)s[i+1] >= 0xA0;
        else if (c >= 0xE1 && c <= 0xEC) ok = cont(2);
        else if (c == 0xED) ok = cont(1) && (unsigned char)s[i+1] <= 0x9F;
        else if (c >= 0xEE && c <= 0xEF) ok = cont(2);
        else if (c == 0xF0) ok = cont(3) && (unsigned char)s[i+1] >= 0x90;
        else if (c >= 0xF1 && c <= 0xF3) ok = cont(3);
        else if (c == 0xF4) ok = cont(3) && (unsigned char)s[i+1] <= 0x8F;
        if (ok){
            size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            o.append(s, i, len); i += len;
        } else {
            o.append(RPL, 3); i++;
        }
    }
    return o;
}

// services a tool may use. keeps tools decoupled from backend types: the
// host supplies callables. add fields here as new shared services appear.
struct tool_context{
    std::function<std::string(const std::string& query)> web_search;
    // the session's current provider/model: subagent children must run on
    // the same provider (a quota-exhausted default would burn every child
    // even when the session was pinned to a healthy one).
    std::function<std::string()> chat_provider;
    std::function<std::string()> chat_model;
};

struct tool{
    std::string name;
    std::string desc; // signature + behavior, rendered into the system prompt
    std::function<std::string(const nlohmann::json& args, tool_context& ctx)> run;
};

// registration is self-service: a tool file declares a static object that
// registers itself at load time. the host only needs to link the .cpp in.
void register_tool(tool t);
// opmem (memory.cpp): the host calls this at the end of every turn to store
// the user's prompt plus a one-line description of what was done. see sysprompt.
void opmem_log(const std::string& prompt, const std::string& summary);
const tool* find_tool(const std::string& name);
std::vector<std::string> tool_names();
std::string render_tools(); // the system prompt section for all registered tools

// name is the tool's dispatch name; fn is a function-like taking
// (const json& args, tool_context& ctx) and returning a string.
#define REGISTER_TOOL_II(toolname, fn, n) static bool _tool_reg_##n = (register_tool({toolname, fn}), true)
#define REGISTER_TOOL_I(toolname, fn, n) REGISTER_TOOL_II(toolname, fn, n)
#define REGISTER_TOOL(toolname, fn) REGISTER_TOOL_I(toolname, fn, __LINE__)

#define REGISTER_TOOL_DESC_III(toolname, fn, desc, n) static bool _tool_reg_##n = (register_tool({toolname, desc, fn}), true)
#define REGISTER_TOOL_DESC_II(toolname, fn, desc, n) REGISTER_TOOL_DESC_III(toolname, fn, desc, n)
#define REGISTER_TOOL_DESC_I(toolname, fn, desc, n) REGISTER_TOOL_DESC_II(toolname, fn, desc, n)
#define REGISTER_TOOL_DESC(toolname, fn, desc) REGISTER_TOOL_DESC_I(toolname, fn, desc, __LINE__)
