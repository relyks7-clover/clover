#include "../tool.h"
#include <string>

static std::string tool_web_search(const nlohmann::json& args, tool_context& ctx){
    if (!args.contains("query"))
        return "error: missing 'query' argument\n";
    if (!ctx.web_search)
        return "error: web_search service unavailable\n";
    return ctx.web_search(args["query"].get<std::string>());
}

REGISTER_TOOL_DESC("web_search", tool_web_search,
R"(web_search(query):
-does a web search on the query and returns results)");
