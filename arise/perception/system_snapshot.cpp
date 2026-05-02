#include "perception/system_snapshot.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;

namespace arise::sys {

namespace {

std::string runCmd(const std::string& cmd) {
    std::string out;
    std::string full = cmd + " 2>/dev/null";
    FILE* p = popen(full.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0)
        out.append(buf, n);
    pclose(p);
    return out;
}

std::string trim(std::string s) {
    auto issp = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && issp(static_cast<unsigned char>(s.back())))  s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && issp(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return trim(ss.str());
}

void sampleNiri(Snapshot& s) {
    auto out = runCmd("niri msg --json focused-window");
    if (out.empty()) return;
    try {
        auto j = json::parse(out);
        // `niri msg --json focused-window` may print either the window object
        // directly or {"FocusedWindow": {...}} depending on niri version.
        if (j.contains("FocusedWindow")) j = j["FocusedWindow"];
        if (j.contains("app_id")  && j["app_id"].is_string())
            s.active_app   = j["app_id"].get<std::string>();
        if (j.contains("title")   && j["title"].is_string())
            s.active_title = j["title"].get<std::string>();
        if (j.contains("workspace_id") && j["workspace_id"].is_number_integer())
            s.workspace_id = j["workspace_id"].get<int>();
    } catch (...) { /* niri not running or different format → leave empty */ }
}

void sampleVolume(Snapshot& s) {
    auto out = runCmd("wpctl get-volume @DEFAULT_AUDIO_SINK@");
    if (out.empty()) return;
    // "Volume: 0.50 [MUTED]"
    std::smatch m;
    std::regex re(R"(Volume:\s*([0-9]*\.?[0-9]+))");
    if (std::regex_search(out, m, re)) {
        try {
            double v = std::stod(m[1].str());
            s.volume_pct = static_cast<int>(v * 100.0 + 0.5);
        } catch (...) {}
    }
    s.muted = out.find("[MUTED]") != std::string::npos;
}

void sampleBrightness(Snapshot& s) {
    // brightnessctl -m → "name,class,current,percent%,max"
    auto out = runCmd("brightnessctl -m");
    if (out.empty()) return;
    auto line = trim(out.substr(0, out.find('\n')));
    std::vector<std::string> parts;
    std::stringstream ss(line);
    for (std::string p; std::getline(ss, p, ',');) parts.push_back(p);
    if (parts.size() >= 4) {
        auto pct = parts[3];
        if (!pct.empty() && pct.back() == '%') pct.pop_back();
        try { s.brightness_pct = std::stoi(pct); } catch (...) {}
    }
}

void sampleBattery(Snapshot& s) {
    // First /sys/class/power_supply/BAT* directory wins.
    std::error_code ec;
    fs::path psd("/sys/class/power_supply");
    if (!fs::exists(psd, ec)) return;
    for (auto& e : fs::directory_iterator(psd, ec)) {
        auto name = e.path().filename().string();
        if (name.rfind("BAT", 0) != 0) continue;
        auto cap = readFile((e.path() / "capacity").string());
        auto st  = readFile((e.path() / "status").string());
        if (!cap.empty()) {
            try { s.battery_pct = std::stoi(cap); } catch (...) {}
        }
        if (!st.empty())  s.battery_status = st;
        break;
    }
}

void sampleNetwork(Snapshot& s) {
    // Try nmcli (most informative); fall back to /sys operstate scan.
    auto nm = runCmd("nmcli -t -f NAME,TYPE,STATE connection show --active");
    if (!nm.empty()) {
        std::stringstream ss(nm);
        for (std::string line; std::getline(ss, line);) {
            // NAME:TYPE:STATE
            if (line.find(":activated") == std::string::npos &&
                line.find(":activating") == std::string::npos) continue;
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            s.network_name = line.substr(0, colon);
            return;
        }
    }
    // Fallback: any non-loopback iface that's "up".
    std::error_code ec;
    fs::path netd("/sys/class/net");
    if (!fs::exists(netd, ec)) return;
    for (auto& e : fs::directory_iterator(netd, ec)) {
        auto name = e.path().filename().string();
        if (name == "lo") continue;
        auto state = readFile((e.path() / "operstate").string());
        if (state == "up") { s.network_name = name; return; }
    }
}

void putOpt(json& j, const char* k, const std::optional<std::string>& v) {
    if (v) j[k] = *v;
}
void putOpt(json& j, const char* k, const std::optional<int>& v) {
    if (v) j[k] = *v;
}
void putOpt(json& j, const char* k, const std::optional<bool>& v) {
    if (v) j[k] = *v;
}

template <class T>
void diffOpt(json& d, const char* k,
             const std::optional<T>& a, const std::optional<T>& b) {
    if (a != b) {
        if (b) d[k] = *b;
        else   d[k] = nullptr;
    }
}

} // namespace

Snapshot take() {
    Snapshot s;
    sampleNiri      (s);
    sampleVolume    (s);
    sampleBrightness(s);
    sampleBattery   (s);
    sampleNetwork   (s);
    return s;
}

json toJson(const Snapshot& s) {
    json j = json::object();
    putOpt(j, "active_app",     s.active_app);
    putOpt(j, "active_title",   s.active_title);
    putOpt(j, "workspace_id",   s.workspace_id);
    putOpt(j, "volume_pct",     s.volume_pct);
    putOpt(j, "muted",          s.muted);
    putOpt(j, "brightness_pct", s.brightness_pct);
    putOpt(j, "battery_pct",    s.battery_pct);
    putOpt(j, "battery_status", s.battery_status);
    putOpt(j, "network_name",   s.network_name);
    return j;
}

json delta(const Snapshot& a, const Snapshot& b) {
    json d = json::object();
    diffOpt(d, "active_app",     a.active_app,     b.active_app);
    diffOpt(d, "active_title",   a.active_title,   b.active_title);
    diffOpt(d, "workspace_id",   a.workspace_id,   b.workspace_id);
    diffOpt(d, "volume_pct",     a.volume_pct,     b.volume_pct);
    diffOpt(d, "muted",          a.muted,          b.muted);
    diffOpt(d, "brightness_pct", a.brightness_pct, b.brightness_pct);
    diffOpt(d, "battery_pct",    a.battery_pct,    b.battery_pct);
    diffOpt(d, "battery_status", a.battery_status, b.battery_status);
    diffOpt(d, "network_name",   a.network_name,   b.network_name);
    return d;
}

} // namespace arise::sys
