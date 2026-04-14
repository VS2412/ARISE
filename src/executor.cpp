#include "executor.hpp"
#include "logger.hpp"
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static std::string q(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}

// Extract a string arg from JSON, with "param" as generic fallback key
static std::string arg(const nlohmann::json& a, const std::string& key) {
    if (a.contains(key) && a[key].is_string()) return a[key].get<std::string>();
    if (a.contains("param") && a["param"].is_string()) return a["param"].get<std::string>();
    return "";
}

static int argInt(const nlohmann::json& a, const std::string& key, int def) {
    if (a.contains(key) && a[key].is_number()) return a[key].get<int>();
    return def;
}

static bool argBool(const nlohmann::json& a, const std::string& key, bool def) {
    if (a.contains(key) && a[key].is_boolean()) return a[key].get<bool>();
    return def;
}

// Expand ~ to $HOME
static std::string expandHome(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
    }
    return path;
}

static const std::vector<std::pair<std::string,std::string>> kAppMap = {
    {"firefox",         "firefox"},
    {"browser",         "firefox"},
    {"chrome",          "chromium"},
    {"chromium",        "chromium"},
    {"terminal",        "alacritty"},
    {"alacritty",       "alacritty"},
    {"kitty",           "kitty"},
    {"code",            "code"},
    {"vscode",          "code"},
    {"vs code",         "code"},
    {"visual studio",   "code"},
    {"spotify",         "spotify"},
    {"discord",         "discord"},
    {"telegram",        "telegram-desktop"},
    {"files",           "nautilus"},
    {"file manager",    "nautilus"},
    {"nautilus",        "nautilus"},
    {"calculator",      "gnome-calculator"},
    {"settings",        "gnome-control-center"},
    {"steam",           "steam"},
    {"obs",             "obs"},
    {"gimp",            "gimp"},
    {"vlc",             "vlc"},
    {"thunar",          "thunar"},
    {"dolphin",         "dolphin"},
    {"blender",         "blender"},
    {"inkscape",        "inkscape"},
};

static std::string resolveApp(const std::string& raw) {
    std::string t = raw;
    std::transform(t.begin(), t.end(), t.begin(), ::tolower);
    for (auto& [k, v] : kAppMap)
        if (t.find(k) != std::string::npos) return v;
    return raw; // pass through verbatim
}

void Executor::shell(const std::string& cmd) {
    Logger::info("Executor: " + cmd);
    system((cmd + " &").c_str());
}

std::string Executor::shellCapture(const std::string& cmd) {
    Logger::info("Executor (capture): " + cmd);
    std::string result;
    FILE* f = popen((cmd + " 2>&1").c_str(), "r");
    if (!f) return "Command failed to execute.";
    char buf[512];
    while (fgets(buf, sizeof(buf), f))
        result += buf;
    int status = pclose(f);
    if (result.empty())
        result = (status == 0) ? "Command completed successfully." : "Command failed.";
    // Truncate long output for LLM context
    if (result.size() > 2000)
        result = result.substr(0, 2000) + "... (truncated)";
    return result;
}

std::string Executor::execute(const AgentAction& action) {
    // ─── Original actions ───
    if (action.type == "open") {
        std::string binary = resolveApp(arg(action.args, "app"));
        if (binary.find(' ') != std::string::npos || binary.empty()) {
            Logger::warn("Executor: rejected unknown app: " + binary);
            return "I don't know how to open that.";
        }
        shell(binary);
        return "On it.";
    }
    if (action.type == "run") {
        std::string cmd = arg(action.args, "command");
        if (cmd.empty()) return "";
        shell(cmd);
        return "Running that now.";
    }
    if (action.type == "type") {
        std::string text = arg(action.args, "text");
        system(("wtype " + q(text)).c_str());
        return "";
    }
    if (action.type == "workspace") {
        std::string num = arg(action.args, "number");
        system(("niri msg action focus-workspace " + q(num)).c_str());
        return "Switching.";
    }
    if (action.type == "close") {
        system("niri msg action close-window");
        return "Closed.";
    }
    if (action.type == "volume") {
        std::string p = arg(action.args, "action");
        if (p == "up") {
            system("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+");
            return "Louder.";
        }
        else if (p == "down") {
            system("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-");
            return "Quieter.";
        }
        else if (p == "mute") {
            system("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
            return "Muted.";
        }
    }
    if (action.type == "brightness") {
        std::string p = arg(action.args, "action");
        if (p == "up") {
            system("brightnessctl set 10%+");
            return "Brighter.";
        }
        else {
            system("brightnessctl set 10%-");
            return "Dimmer.";
        }
    }
    if (action.type == "media") {
        std::string p = arg(action.args, "action");
        if      (p == "play" || p == "pause") system("playerctl play-pause");
        else if (p == "next")  system("playerctl next");
        else if (p == "prev")  system("playerctl previous");
        else if (p == "stop")  system("playerctl stop");
        return "";
    }
    if (action.type == "screenshot") {
        system("grim ~/Pictures/screenshot-$(date +%Y%m%d-%H%M%S).png 2>/dev/null");
        return "Screenshot saved.";
    }
    if (action.type == "clipboard") {
        std::string p = arg(action.args, "action");
        if (p == "read") {
            FILE* f = popen("wl-paste --no-newline 2>/dev/null | head -c 200", "r");
            if (!f) return "Clipboard empty.";
            char buf[256] = {}; fread(buf, 1, 255, f); pclose(f);
            return std::string(buf).empty() ? "Clipboard is empty." : std::string(buf);
        }
        if (p == "write") {
            std::string text = arg(action.args, "text");
            system(("echo " + q(text) + " | wl-copy").c_str());
            return "Copied.";
        }
    }
    if (action.type == "notify") {
        std::string msg = arg(action.args, "message");
        system(("notify-send 'ARIA' " + q(msg)).c_str());
        return "";
    }
    if (action.type == "url") {
        std::string url = arg(action.args, "url");
        shell("xdg-open " + q(url));
        return "Opening link.";
    }
    if (action.type == "sequence") {
        std::string steps = arg(action.args, "steps");
        std::istringstream ss(steps);
        std::string step;
        while (std::getline(ss, step, '|')) {
            auto sp = step.find(' ');
            AgentAction sub;
            sub.type = (sp == std::string::npos) ? step : step.substr(0, sp);
            sub.args = {{"param", (sp == std::string::npos) ? "" : step.substr(sp + 1)}};
            execute(sub);
        }
        return "Done.";
    }

    // ─── Phase 2A: File operations ───
    if (action.type == "file_read") {
        std::string path = expandHome(arg(action.args, "path"));
        int maxLines = argInt(action.args, "max_lines", 50);
        if (path.empty()) return "No path specified.";
        return shellCapture("head -n " + std::to_string(maxLines) + " " + q(path));
    }
    if (action.type == "file_write") {
        std::string path = expandHome(arg(action.args, "path"));
        std::string content = arg(action.args, "content");
        bool append = argBool(action.args, "append", false);
        if (path.empty()) return "No path specified.";
        // Use C++ file I/O to avoid shell injection
        auto mode = append ? (std::ios::out | std::ios::app) : std::ios::out;
        std::ofstream f(path, mode);
        if (!f) return "Cannot write to " + path;
        f << content;
        f.close();
        Logger::info("Executor: wrote " + std::to_string(content.size()) + " bytes to " + path);
        return "File written.";
    }
    if (action.type == "file_search") {
        std::string dir = expandHome(arg(action.args, "directory"));
        if (dir.empty()) dir = expandHome("~");
        std::string pattern = arg(action.args, "pattern");
        std::string grep = arg(action.args, "content_grep");
        if (pattern.empty()) return "No search pattern specified.";
        std::string cmd = "find " + q(dir) + " -maxdepth 5 -name " + q(pattern) + " 2>/dev/null | head -20";
        if (!grep.empty())
            cmd = "find " + q(dir) + " -maxdepth 5 -name " + q(pattern) +
                  " -exec grep -l " + q(grep) + " {} + 2>/dev/null | head -20";
        return shellCapture(cmd);
    }
    if (action.type == "file_list") {
        std::string path = expandHome(arg(action.args, "path"));
        if (path.empty()) path = expandHome("~");
        return shellCapture("ls -lah " + q(path) + " | head -30");
    }

    // ─── Phase 2B: Window management ───
    if (action.type == "window") {
        std::string act = arg(action.args, "action");
        if (act.empty()) return "No window action specified.";
        // Map friendly names to niri action names
        std::string niriAction = act;
        if (act == "maximize") niriAction = "maximize-column";
        else if (act == "fullscreen") niriAction = "fullscreen-window";
        else if (act == "center") niriAction = "center-column";
        system(("niri msg action " + q(niriAction) + " 2>/dev/null").c_str());
        return "Done.";
    }
    if (action.type == "list_windows") {
        std::string raw = shellCapture("niri msg --json windows 2>/dev/null");
        try {
            auto windows = json::parse(raw);
            std::ostringstream out;
            for (auto& w : windows) {
                std::string app = w.value("app_id", "unknown");
                std::string title = w.value("title", "");
                if (title.size() > 60) title = title.substr(0, 60) + "...";
                out << app << ": " << title << "\n";
            }
            std::string result = out.str();
            return result.empty() ? "No windows open." : result;
        } catch (...) {
            return raw; // return raw output if JSON parse fails
        }
    }
    if (action.type == "focus_window") {
        std::string app = arg(action.args, "app");
        if (app.empty()) return "No app specified.";
        std::string appLower = app;
        std::transform(appLower.begin(), appLower.end(), appLower.begin(), ::tolower);
        // Find window ID by app name
        std::string raw = shellCapture("niri msg --json windows 2>/dev/null");
        try {
            auto windows = json::parse(raw);
            for (auto& w : windows) {
                std::string wApp = w.value("app_id", "");
                std::string wAppLower = wApp;
                std::transform(wAppLower.begin(), wAppLower.end(), wAppLower.begin(), ::tolower);
                if (wAppLower.find(appLower) != std::string::npos) {
                    int id = w.value("id", 0);
                    if (id > 0) {
                        system(("niri msg action focus-window --id " + std::to_string(id) + " 2>/dev/null").c_str());
                        return "Focused " + wApp + ".";
                    }
                }
            }
        } catch (...) {}
        return "Couldn't find a window for " + app + ".";
    }

    // ─── Phase 2C: Process management ───
    if (action.type == "proc_list") {
        std::string filter = arg(action.args, "filter");
        if (filter.empty())
            return shellCapture("ps aux --sort=-%mem | head -15");
        return shellCapture("ps aux | grep -i " + q(filter) + " | grep -v grep | head -15");
    }
    if (action.type == "proc_kill") {
        std::string target = arg(action.args, "target");
        if (target.empty()) return "No process specified.";
        // Check if it's a PID (all digits) or a name
        bool isPid = std::all_of(target.begin(), target.end(), ::isdigit);
        if (isPid) {
            return shellCapture("kill " + target);
        }
        return shellCapture("pkill -f " + q(target));
    }

    // ─── Phase 2D: Timers (delegated to daemon's TimerManager) ───
    if (action.type == "timer_set" || action.type == "timer_list" || action.type == "timer_cancel") {
        // These are handled by the daemon, not the executor
        // Return a marker so the daemon knows to handle it
        return "__TIMER__";
    }

    // ─── Phase 2E: Web search ───
    if (action.type == "web_search") {
        std::string query = arg(action.args, "query");
        if (query.empty()) return "No query specified.";
        // Use DuckDuckGo lite HTML and extract results
        std::string cmd = "curl -s 'https://lite.duckduckgo.com/lite/?q=" + q(query).substr(1, q(query).size()-2) + "'"
                          " | sed -n 's/.*<a[^>]*href=\"\\([^\"]*\\)\"[^>]*class=\"result-link\"[^>]*>\\(.*\\)<\\/a>.*/\\2: \\1/p'"
                          " | head -5 2>/dev/null";
        std::string result = shellCapture(cmd);
        if (result.empty() || result == "Command completed successfully.") {
            // Fallback: use curl to DuckDuckGo API
            std::string escaped;
            for (char c : query) {
                if (c == ' ') escaped += '+';
                else escaped += c;
            }
            result = shellCapture("curl -s 'https://api.duckduckgo.com/?q=" + escaped + "&format=json&no_html=1' "
                                  "| python3 -c \"import sys,json; d=json.load(sys.stdin); "
                                  "[print(r.get('Text','')[:200]) for r in (d.get('RelatedTopics',[]))[:5] if r.get('Text')]\" 2>/dev/null");
        }
        if (result.empty() || result == "Command completed successfully.")
            return "No results found. Try opening a browser search.";
        return result;
    }

    // ─── Phase 2F: System diagnostics ───
    if (action.type == "sysinfo") {
        std::string cat = arg(action.args, "category");
        if (cat.empty()) cat = "all";
        std::string result;
        if (cat == "disk" || cat == "all")
            result += "DISK:\n" + shellCapture("df -h --output=target,size,avail,pcent / /home 2>/dev/null") + "\n";
        if (cat == "memory" || cat == "all")
            result += "MEMORY:\n" + shellCapture("free -h") + "\n";
        if (cat == "cpu" || cat == "all")
            result += "CPU:\n" + shellCapture("top -bn1 | head -5") + "\n";
        if (cat == "battery" || cat == "all") {
            std::string bat = shellCapture("cat /sys/class/power_supply/BAT*/capacity 2>/dev/null");
            if (bat.find("No such file") == std::string::npos && !bat.empty())
                result += "BATTERY: " + bat + "%\n";
        }
        if (cat == "network" || cat == "all")
            result += "NETWORK:\n" + shellCapture("ip -br addr 2>/dev/null | head -5") + "\n";
        return result;
    }

    Logger::warn("Executor: unknown action: " + action.type);
    return "";
}
