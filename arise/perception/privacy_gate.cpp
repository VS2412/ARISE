#include "perception/privacy_gate.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

namespace arise {

namespace {

std::string runCmd(const std::string& cmd) {
    std::string out;
    std::string full = cmd + " 2>/dev/null";
    FILE* p = popen(full.c_str(), "r");
    if (!p) return out;
    char buf[2048];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

// Default probe: ask niri for the focused window's app_id. Empty string on
// any failure (niri missing, no window focused, JSON parse error).
std::string niriProbe() {
    auto out = runCmd("niri msg --json focused-window");
    if (out.empty()) return {};
    try {
        auto j = nlohmann::json::parse(out);
        if (j.contains("FocusedWindow")) j = j["FocusedWindow"];
        if (j.contains("app_id") && j["app_id"].is_string())
            return j["app_id"].get<std::string>();
    } catch (...) {}
    return {};
}

} // namespace

PrivacyGate::PrivacyGate(Config cfg)
    : cfg_(std::move(cfg)),
      probe_(&niriProbe) {}

void PrivacyGate::setProbe(Probe p) {
    std::lock_guard<std::mutex> lk(mu_);
    probe_ = std::move(p);
    last_check_ = {};                    // bust cache so the new probe is hit
}

bool PrivacyGate::wouldBlock(const std::string& app_id) const {
    if (cfg_.private_apps.empty() || app_id.empty()) return false;
    return std::find(cfg_.private_apps.begin(), cfg_.private_apps.end(), app_id)
           != cfg_.private_apps.end();
}

bool PrivacyGate::isPrivate() {
    if (cfg_.private_apps.empty()) return false;

    std::lock_guard<std::mutex> lk(mu_);
    auto now = std::chrono::steady_clock::now();
    if (last_check_ != std::chrono::steady_clock::time_point{} &&
        now - last_check_ < cfg_.cache_ttl) {
        return last_private_;
    }

    auto cur = probe_ ? probe_() : std::string{};
    last_check_ = now;
    last_match_.clear();

    if (cur.empty()) {
        last_private_ = cfg_.failsafe_private_on_probe_error;
        return last_private_;
    }
    auto it = std::find(cfg_.private_apps.begin(), cfg_.private_apps.end(), cur);
    if (it != cfg_.private_apps.end()) {
        last_match_  = *it;
        last_private_ = true;
    } else {
        last_private_ = false;
    }
    return last_private_;
}

std::string PrivacyGate::lastMatched() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_match_;
}

} // namespace arise
