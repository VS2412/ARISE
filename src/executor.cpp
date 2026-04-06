#include "executor.hpp"
#include "logger.hpp"
#include <vector>
#include <cstdlib>
#include <sstream>

static std::string q(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}

void Executor::shell(const std::string& cmd) {
    Logger::info("Executor: " + cmd);
    system((cmd + " &").c_str());
}

std::string Executor::execute(const AgentAction& action) {
    const auto& p = action.param;

    if (action.type == "open") {
        // web-only apps — open as URLs instead of binaries
        static const std::vector<std::pair<std::string,std::string>> webApps = {
            {"whatsapp",   "https://web.whatsapp.com"},
            {"youtube",    "https://youtube.com"},
            {"gmail",      "https://mail.google.com"},
            {"instagram",  "https://instagram.com"},
            {"twitter",    "https://twitter.com"},
            {"reddit",     "https://reddit.com"},
            {"github",     "https://github.com"},
            {"linkedin",   "https://linkedin.com"},
            {"netflix",    "https://netflix.com"},
            {"wikipedia",  "https://wikipedia.org"},
        };
        for (auto& [k, v] : webApps) {
            if (p.find(k) != std::string::npos) {
                shell("xdg-open " + v);
                return "Opening " + k + ".";
            }
        }
        shell(p);
        return "Opening " + p + ".";
    }
    if (action.type == "run") {
        shell(p);
        return "Done.";
    }
    if (action.type == "type") {
        system(("wtype " + q(p)).c_str());
        return "";
    }
    if (action.type == "workspace") {
        system(("niri msg action focus-workspace " + q(p)).c_str());
        return "Workspace " + p + ".";
    }
    if (action.type == "volume") {
        if      (p == "up")   system("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+");
        else if (p == "down") system("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-");
        else if (p == "mute") system("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
        return "Volume " + p + ".";
    }
    if (action.type == "close") {
        system("niri msg action close-window");
        return "Closed.";
    }
    if (action.type == "screenshot") {
        system("grim ~/Pictures/screenshot-$(date +%Y%m%d-%H%M%S).png 2>/dev/null"
               " || scrot ~/Pictures/screenshot-%Y%m%d-%H%M%S.png 2>/dev/null");
        return "Screenshot saved.";
    }
    if (action.type == "brightness") {
        if (p == "up")   system("brightnessctl set 10%+");
        else             system("brightnessctl set 10%-");
        return "Brightness " + p + ".";
    }
    if (action.type == "media") {
        if      (p == "play" || p == "pause") system("playerctl play-pause");
        else if (p == "next")  system("playerctl next");
        else if (p == "prev")  system("playerctl previous");
        else if (p == "stop")  system("playerctl stop");
        return "";
    }
    if (action.type == "clipboard") {
        if (p == "read") {
            FILE* f = popen("wl-paste --no-newline 2>/dev/null | head -c 200", "r");
            if (!f) return "Clipboard empty.";
            char buf[256] = {}; fread(buf, 1, 255, f); pclose(f);
            return std::string(buf);
        }
        if (p.substr(0, 6) == "write:") {
            std::string text = p.substr(6);
            system(("echo " + q(text) + " | wl-copy").c_str());
            return "Copied.";
        }
    }
    if (action.type == "notify") {
        system(("notify-send 'ARIA' " + q(p)).c_str());
        return "";
    }
    if (action.type == "url") {
        shell("xdg-open " + q(p));
        return "Opening link.";
    }
    // sequence: pipe-separated actions e.g. "open firefox|workspace 2"
    if (action.type == "sequence") {
        std::istringstream ss(p);
        std::string step;
        while (std::getline(ss, step, '|')) {
            auto space = step.find(' ');
            AgentAction sub;
            sub.type  = (space == std::string::npos) ? step : step.substr(0, space);
            sub.param = (space == std::string::npos) ? ""   : step.substr(space + 1);
            execute(sub);
        }
        return "Done.";
    }

    Logger::warn("Executor: unknown action: " + action.type);
    return "";
}