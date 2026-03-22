#include "executor.hpp"
#include "logger.hpp"
#include <cstdlib>

static std::string quoted(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

void Executor::shell(const std::string& cmd) {
    Logger::info("Executor: " + cmd);
    system(cmd.c_str());
}

std::string Executor::execute(const AgentAction& action) {
    const auto& p = action.param;

    if (action.type == "open") {
        shell(p + " & disown");
        return "Opening " + p + ".";
    }
    if (action.type == "run") {
        shell(p + " & disown");
        return "Running command.";
    }
    if (action.type == "type") {
        shell("wtype " + quoted(p));
        return "";
    }
    if (action.type == "workspace") {
        shell("niri msg action focus-workspace " + quoted(p));
        return "Switched to workspace " + p + ".";
    }
    if (action.type == "volume") {
        if      (p == "up")   shell("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+");
        else if (p == "down") shell("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-");
        else if (p == "mute") shell("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
        return "Volume " + p + ".";
    }

    Logger::warn("Executor: unknown action: " + action.type);
    return "";
}