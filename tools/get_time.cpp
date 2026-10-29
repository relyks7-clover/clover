#include "../tool.h"
#include <ctime>
#include <chrono>
#include <string>

static std::string tool_get_time(const nlohmann::json&, tool_context&){
    auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm tmv;
    gmtime_r(&t,&tmv);
    char buf[64];
    strftime(buf,sizeof(buf),"%a %b %e %H:%M:%S %Y\n",&tmv);
    return std::string("current time: ")+buf;
}

REGISTER_TOOL_DESC("get_time", tool_get_time,
R"(get_time():
-returns the current time in utc
-this tool has no arguments)");
