#include "cortex/proactive.hpp"

#include "blackboard/blackboard.hpp"
#include "cortex/memory_cortex.hpp"
#include "util/log.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

Tier severityToTier(const std::string& sev) {
    if (sev == "urgent")  return Tier::Urgent;
    if (sev == "active")  return Tier::Active;
    if (sev == "ambient") return Tier::Ambient;
    return Tier::Silent;
}

double salienceForTier(Tier t) {
    switch (t) {
        case Tier::Silent:  return 0.1;
        case Tier::Ambient: return 0.35;
        case Tier::Active:  return 0.6;
        case Tier::Urgent:  return 0.85;
    }
    return 0.3;
}

} // namespace

// ─── pure builders ─────────────────────────────────────────────────────────

Suggestion ProactiveEngine::buildFromWatcherNotice(const json& payload,
                                                   const std::string& topic) {
    Suggestion s;
    s.source_topic   = topic;
    s.source_payload = payload;
    s.tier           = severityToTier(payload.value("severity", std::string{}));
    s.category       = payload.value("kind", std::string{"watcher"});
    s.text           = payload.value("summary", std::string{});
    return s;
}

Suggestion ProactiveEngine::buildFromGoalEvent(const json& payload,
                                               const std::string& topic) {
    Suggestion s;
    s.source_topic   = topic;
    s.source_payload = payload;
    auto summary = payload.value("summary", std::string{"goal"});

    if (topic == "goal.escalated") {
        s.tier     = Tier::Active;
        s.category = "goal_escalated";
        s.text     = "deadline coming up: " + summary;
    } else if (topic == "goal.due") {
        s.tier     = Tier::Ambient;
        s.category = "goal_due";
        s.text     = "due soon: " + summary;
    } else if (topic == "goal.stale") {
        s.tier     = Tier::Ambient;
        s.category = "goal_stale";
        long long stale_s = 0;
        try {
            // payload value is a string per the scheduler.
            stale_s = std::stoll(payload.value("stale_seconds", std::string{"0"}));
        } catch (...) {}
        if (stale_s > 0) {
            int days = int(stale_s / 86400);
            s.text = (days > 0
                       ? std::to_string(days) + "d "
                       : std::string{})
                   + "no progress on: " + summary;
        } else {
            s.text = "stalled goal: " + summary;
        }
    }
    return s;
}

Suggestion ProactiveEngine::buildFromAudioScene(const json& payload,
                                                const std::string& topic) {
    Suggestion s;
    s.source_topic   = topic;
    s.source_payload = payload;
    auto scene = payload.value("scene", std::string{});
    if (scene == "alarm") {
        s.tier = Tier::Urgent;  s.category = "alarm_heard";
        s.text = "alarm sound detected — check the room";
    } else if (scene == "doorbell") {
        s.tier = Tier::Active;  s.category = "doorbell_heard";
        s.text = "doorbell — someone at the door";
    } else if (scene == "phone") {
        s.tier = Tier::Active;  s.category = "phone_heard";
        s.text = "phone ringing nearby";
    }
    return s;
}

Suggestion ProactiveEngine::buildCandidate(const std::string& topic,
                                           const json& payload) {
    if (topic == "agent.watcher.notice") return buildFromWatcherNotice(payload, topic);
    if (topic == "goal.due" || topic == "goal.escalated" || topic == "goal.stale")
        return buildFromGoalEvent(payload, topic);
    if (topic == "audio.scene_changed" || topic == "audio.scene_first")
        return buildFromAudioScene(payload, topic);
    return Suggestion{};      // tier defaults to Ambient but text empty → ignored
}

// ─── impl ──────────────────────────────────────────────────────────────────

struct ProactiveEngine::Impl {
    Config             cfg;
    SuggestionGate     gate;

    std::atomic<bool>  running { false };
    std::atomic<bool>  stopping{ false };
    std::thread        worker;

    Blackboard::Subscription sub;

    mutable std::mutex stats_mu;
    Stats              stats;

    explicit Impl(Config c) : cfg(std::move(c)), gate(cfg.gate) {}

    void incBlock(GateOutcome o) {
        std::lock_guard<std::mutex> lk(stats_mu);
        switch (o) {
            case GateOutcome::BlockedRateLimit:     ++stats.blocked_rate;   break;
            case GateOutcome::BlockedQuietHours:    ++stats.blocked_quiet;  break;
            case GateOutcome::BlockedCategoryMuted: ++stats.blocked_muted;  break;
            case GateOutcome::BlockedSilent:        ++stats.blocked_silent; break;
            default: break;
        }
    }
};

ProactiveEngine::ProactiveEngine(Config cfg) : p_(std::make_unique<Impl>(std::move(cfg))) {
    if (!p_->cfg.bb)       log::error("ProactiveEngine: bb is required");
    if (!p_->cfg.feedback) log::error("ProactiveEngine: feedback is required");
}
ProactiveEngine::~ProactiveEngine() { stop(); }

bool ProactiveEngine::running() const { return p_->running.load(); }

ProactiveEngine::Stats ProactiveEngine::stats() const {
    std::lock_guard<std::mutex> lk(p_->stats_mu);
    return p_->stats;
}

void ProactiveEngine::muteCategory(const std::string& category,
                                   std::chrono::seconds duration) {
    p_->gate.muteCategory(category, duration);
}

void ProactiveEngine::start() {
    if (!p_->cfg.bb || !p_->cfg.feedback) return;
    bool expected = false;
    if (!p_->running.compare_exchange_strong(expected, true)) return;
    p_->stopping.store(false);
    p_->sub = p_->cfg.bb->subscribe("");      // wildcard; filter inside
    p_->worker = std::thread(&ProactiveEngine::workerLoop_, this);
    log::info("ProactiveEngine: started");
}

void ProactiveEngine::stop() {
    if (!p_) return;
    bool was_running = p_->running.exchange(false);
    p_->stopping.store(true);
    if (p_->sub.valid()) p_->sub.stop();
    if (p_->worker.joinable()) p_->worker.join();
    if (was_running) log::info("ProactiveEngine: stopped");
}

void ProactiveEngine::workerLoop_() {
    while (!p_->stopping.load()) {
        auto ev = p_->sub.next(milliseconds(250));
        if (!ev) continue;
        { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.signals_seen; }
        // Don't recurse on our own emissions.
        if (ev->topic == "proactive.suggestion" ||
            ev->topic == "proactive.dropped") continue;
        evaluate(ev->topic, ev->payload);
    }
}

ProactiveEngine::EvalResult
ProactiveEngine::evaluate(const std::string& topic, const json& payload) {
    EvalResult out;
    auto s = buildCandidate(topic, payload);
    // Empty text means the topic isn't actionable. Tier defaults to Ambient
    // even for unknown topics (struct default), so we can't gate solely on
    // tier — text is the user-facing payload and the only thing that
    // matters here.
    if (s.text.empty()) return out;
    out.suggestion = s;
    { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.suggested; }

    // Sync feedback's per-category reject streak into the gate so
    // auto-mute is consistent across restarts.
    if (p_->cfg.feedback && !s.category.empty()) {
        int streak = p_->cfg.feedback->consecutiveRejects(s.category);
        p_->gate.setConsecutiveRejects(s.category, streak);
    }

    auto now = system_clock::now();
    auto outcome = p_->gate.check(s, now);
    out.outcome = outcome;

    if (outcome != GateOutcome::Pass) {
        p_->incBlock(outcome);
        if (p_->cfg.publish_dropped && p_->cfg.bb) {
            json dp;
            dp["category"]      = s.category;
            dp["tier"]          = tierToString(s.tier);
            dp["text"]          = s.text;
            dp["source_topic"]  = s.source_topic;
            dp["outcome"]       = gateOutcomeToString(outcome);
            p_->cfg.bb->publish("proactive.dropped", dp);
        }
        return out;
    }

    // Persist + publish.
    out.suggestion.proposed_at = now;
    if (p_->cfg.feedback) p_->cfg.feedback->recordProposed(out.suggestion);

    p_->gate.noteFired(out.suggestion.tier, now);
    { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.passed; }

    if (p_->cfg.bb) {
        json payload_out;
        payload_out["id"]            = out.suggestion.id;
        payload_out["tier"]          = tierToString(out.suggestion.tier);
        payload_out["category"]      = out.suggestion.category;
        payload_out["text"]          = out.suggestion.text;
        payload_out["source_topic"]  = out.suggestion.source_topic;
        p_->cfg.bb->publish("proactive.suggestion", payload_out);
    }

    if (p_->cfg.cortex) {
        EpisodicEvent ev;
        ev.kind     = "proactive_suggestion";
        ev.summary  = "[" + std::string(tierToString(out.suggestion.tier)) + "] "
                    + out.suggestion.text;
        ev.salience = salienceForTier(out.suggestion.tier);
        json payload_persist;
        payload_persist["id"]           = out.suggestion.id;
        payload_persist["category"]     = out.suggestion.category;
        payload_persist["source_topic"] = out.suggestion.source_topic;
        ev.payload_json = payload_persist.dump();
        p_->cfg.cortex->recordEvent(std::move(ev));
    }

    out.published = true;
    return out;
}

} // namespace arise
