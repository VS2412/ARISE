#include "cortex/watcher.hpp"

#include "blackboard/blackboard.hpp"
#include "cortex/goals.hpp"
#include "cortex/memory_cortex.hpp"
#include "util/log.hpp"

#include <atomic>
#include <chrono>
#include <cctype>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace std::chrono;

namespace arise {

const char* Watcher::severityToString(Severity s) {
    switch (s) {
        case Severity::Silent:  return "silent";
        case Severity::Ambient: return "ambient";
        case Severity::Active:  return "active";
        case Severity::Urgent:  return "urgent";
    }
    return "silent";
}

namespace {

std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

double salienceForSeverity(Watcher::Severity s) {
    switch (s) {
        case Watcher::Severity::Silent:  return 0.1;
        case Watcher::Severity::Ambient: return 0.35;
        case Watcher::Severity::Active:  return 0.6;
        case Watcher::Severity::Urgent:  return 0.85;
    }
    return 0.3;
}

} // namespace

// ─── impl ──────────────────────────────────────────────────────────────────

struct Watcher::Impl {
    Config cfg;

    std::atomic<bool> running { false };
    std::atomic<bool> stopping{ false };
    std::thread       worker;

    Blackboard::Subscription sub;

    mutable std::mutex stats_mu;
    Stats              stats;
};

Watcher::Watcher(Config cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = std::move(cfg);
    if (!p_->cfg.bb) log::error("Watcher: bb is required");
}
Watcher::~Watcher() { stop(); }

bool Watcher::running() const { return p_->running.load(); }
Watcher::Stats Watcher::stats() const {
    std::lock_guard<std::mutex> lk(p_->stats_mu);
    return p_->stats;
}

void Watcher::start() {
    if (!p_->cfg.bb) return;
    bool expected = false;
    if (!p_->running.compare_exchange_strong(expected, true)) return;
    p_->stopping.store(false);
    p_->sub = p_->cfg.bb->subscribe("");        // wildcard; we filter inside
    p_->worker = std::thread(&Watcher::workerLoop_, this);
    log::info("Watcher: started");
}

void Watcher::stop() {
    if (!p_) return;
    bool was_running = p_->running.exchange(false);
    p_->stopping.store(true);
    if (p_->sub.valid()) p_->sub.stop();
    if (p_->worker.joinable()) p_->worker.join();
    if (was_running) log::info("Watcher: stopped");
}

void Watcher::workerLoop_() {
    while (!p_->stopping.load()) {
        auto ev = p_->sub.next(milliseconds(250));
        if (!ev) continue;
        { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.events_seen; }

        if (ev->topic == "system.snapshot" || ev->topic == "system.delta") {
            if (ev->payload.contains("battery_pct")
                && ev->payload["battery_pct"].is_number()) {
                int pct = ev->payload["battery_pct"].get<int>();
                applyDecision(evaluateBatteryPct(pct), ev->topic);
            }
        } else if (ev->topic == "vision.caption"
                   && ev->payload.contains("caption")
                   && ev->payload["caption"].is_string()) {
            applyDecision(evaluateCaption(ev->payload["caption"].get<std::string>()),
                          ev->topic);
        } else if (ev->topic == "audio.scene_first"
                   || ev->topic == "audio.scene_changed") {
            if (ev->payload.contains("scene") && ev->payload["scene"].is_string()) {
                applyDecision(
                    evaluateAudioScene(ev->payload["scene"].get<std::string>()),
                    ev->topic);
            }
        }
        // Other topics: ignored.
    }
}

// ─── pure rules ────────────────────────────────────────────────────────────

Watcher::Decision Watcher::evaluateBatteryPct(int pct) const {
    Decision d;
    if (pct <= p_->cfg.battery_critical_pct) {
        d.severity     = Severity::Urgent;
        d.kind         = "battery_critical";
        d.summary      = "battery " + std::to_string(pct) + "% — plug in now";
        d.propose_goal = p_->cfg.propose_goals_on_critical && p_->cfg.goals;
        d.goal_summary = "plug in laptop (battery at " + std::to_string(pct) + "%)";
        d.goal_priority = 90;
    } else if (pct <= p_->cfg.battery_warning_pct) {
        d.severity = Severity::Ambient;
        d.kind     = "battery_warning";
        d.summary  = "battery " + std::to_string(pct) + "% — plug in soon";
    }
    return d;                                   // Silent by default
}

Watcher::Decision Watcher::evaluateCaption(const std::string& caption) const {
    Decision d;
    auto lower = toLower(caption);
    for (const auto& kw : p_->cfg.error_keywords) {
        auto kw_l = toLower(kw);
        if (lower.find(kw_l) != std::string::npos) {
            d.severity = Severity::Active;
            d.kind     = "visible_error";
            d.summary  = "screen mentions \"" + kw + "\" — " + caption;
            return d;
        }
    }
    return d;
}

Watcher::Decision Watcher::evaluateAudioScene(const std::string& scene) const {
    Decision d;
    if (scene == "alarm") {
        d.severity = Severity::Urgent;
        d.kind     = "alarm_heard";
        d.summary  = "alarm sound detected";
    } else if (scene == "doorbell") {
        d.severity = Severity::Active;
        d.kind     = "doorbell_heard";
        d.summary  = "doorbell";
    } else if (scene == "phone") {
        d.severity = Severity::Active;
        d.kind     = "phone_heard";
        d.summary  = "phone ring detected";
    }
    // speech / music / typing / silence / other → Silent, no notice
    return d;
}

// ─── side-effecting publish + propose ──────────────────────────────────────

Watcher::Decision Watcher::applyDecision(Decision d, const std::string& source_topic) {
    if (d.severity == Severity::Silent && !p_->cfg.publish_silent_notices) {
        return d;
    }
    if (!p_->cfg.bb) return d;

    json payload = {
        {"kind",     d.kind},
        {"severity", severityToString(d.severity)},
        {"summary",  d.summary},
        {"source",   source_topic},
    };

    if (d.propose_goal && p_->cfg.goals && !d.goal_summary.empty()) {
        Goal g;
        g.summary  = d.goal_summary;
        g.priority = d.goal_priority;
        g.tags     = {"watcher", d.kind};
        auto id = p_->cfg.goals->propose(std::move(g));
        if (id > 0) {
            payload["goal_id"] = id;
            std::lock_guard<std::mutex> lk(p_->stats_mu);
            ++p_->stats.goals_proposed;
        }
    }

    p_->cfg.bb->publish("agent.watcher.notice", payload);
    { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.notices_emitted; }

    if (p_->cfg.cortex) {
        EpisodicEvent ev;
        ev.kind     = "watcher_notice";
        ev.summary  = "[" + d.kind + "] " + d.summary;
        ev.salience = salienceForSeverity(d.severity);
        ev.payload_json = payload.dump();
        p_->cfg.cortex->recordEvent(std::move(ev));
    }
    return d;
}

} // namespace arise
