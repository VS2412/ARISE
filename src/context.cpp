#include "context.hpp"
#include <cstdio>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static std::string exec(const char* cmd) {
    std::string result;
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd, "r"), pclose);
    // std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) return "";
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe.get()))
        result += buf;
    if (!result.empty() && result.back() == '\n')
        result.pop_back();
    return result;
}

SystemContext Context::capture() {
    SystemContext ctx;

    // active window via niri IPC
    try {
        std::string raw = exec("niri msg --json focused-window 2>/dev/null");
        if (!raw.empty()) {
            auto j = json::parse(raw);
            ctx.activeApp    = j.value("app_id", "");
            ctx.activeWindow = j.value("title",  "");
        }
    } catch (...) {}

    // clipboard — truncate to 120 chars to save tokens
    ctx.clipboard = exec("wl-paste --no-newline 2>/dev/null | head -c 120");

    return ctx;
}