#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace arise {

// 4-tier suggestion ladder driving the proactive engine. Mapped from
// blackboard signal severity; the gate decides whether each tier actually
// reaches the user right now.
//
//   Silent   — log only, never speak
//   Ambient  — one-line subtle nudge ("disk is at 95%")
//   Active   — concrete offer ("want me to clean ~/Downloads?")
//   Urgent   — speak even if user is busy (battery 5%, alarm, security alert)
enum class Tier {
    Silent,
    Ambient,
    Active,
    Urgent,
};

const char*        tierToString(Tier t);
std::optional<Tier> tierFromString(std::string_view s);
int                 tierRank(Tier t);   // Silent=0 < Ambient=1 < Active=2 < Urgent=3

// What a Suggestion looks like once the engine has decided to propose it.
// `id` is assigned by FeedbackDb on persist; everything else is set by the
// trigger that built the Suggestion.
struct Suggestion {
    std::int64_t        id = 0;            // 0 until persisted
    Tier                tier = Tier::Ambient;
    std::string         category;          // e.g. "battery_critical", "goal_due"
    std::string         text;              // user-facing one-liner
    std::string         source_topic;      // blackboard topic that triggered
    nlohmann::json      source_payload;    // original payload (for audit / replay)

    // Stamped by the engine right before sending to the gate.
    std::chrono::system_clock::time_point proposed_at;
};

// Reasons the gate may pass or block a suggestion.
enum class GateOutcome {
    Pass,                  // accepted; engine should publish + persist
    BlockedRateLimit,      // tier-specific min interval not yet elapsed
    BlockedQuietHours,     // ambient/active suppressed during quiet hours
    BlockedCategoryMuted,  // category auto-muted by feedback loop
    BlockedSilent,         // suggestion was Tier::Silent — log-only
};

const char* gateOutcomeToString(GateOutcome o);

// Configurable rate limiter / quiet-hours / per-category mute. No state
// outside the in-memory tables here — feedback persistence lives in
// FeedbackDb. The engine threads the two together.
class SuggestionGate {
public:
    struct Config {
        std::chrono::seconds min_interval_ambient { 60 };       // 1 / min
        std::chrono::seconds min_interval_active  { 300 };      // 1 / 5 min
        std::chrono::seconds min_interval_urgent  { 0 };        // unrestricted

        bool quiet_hours_enabled = false;
        int  quiet_start_hour    = 23;        // 23:00 inclusive
        int  quiet_end_hour      = 7;         // 07:00 exclusive

        // Category auto-mute: when consecutive_rejects(category) crosses this
        // threshold, suggestions of that category are blocked for the
        // configured window. Reset by any non-reject decision.
        int                  mute_after_rejects   = 5;
        std::chrono::seconds mute_window          { 24 * 3600 };
    };

    explicit SuggestionGate(Config cfg);

    // Pure check: does this suggestion pass given current time + state?
    // `now` overrides system_clock::now() for tests.
    GateOutcome check(const Suggestion& s,
                      std::chrono::system_clock::time_point now =
                          std::chrono::system_clock::now()) const;

    // Stamps the gate's "last fired at" tier table. Call this when the
    // engine publishes the suggestion (i.e. after Pass). No-op for Silent /
    // Urgent (Urgent doesn't burn a budget either way).
    void noteFired(Tier t,
                   std::chrono::system_clock::time_point now =
                       std::chrono::system_clock::now());

    // Manual mute of a category. Pass duration {0} to clear.
    void muteCategory(const std::string& category,
                      std::chrono::seconds duration);

    // Inject feedback counters from the persistence layer. The engine
    // typically calls this once on startup and after each user decision.
    void setConsecutiveRejects(const std::string& category, int n);

    int  consecutiveRejects(const std::string& category) const;

    // Helpers for diagnostics / tests.
    bool isInQuietHours(std::chrono::system_clock::time_point t) const;
    std::chrono::system_clock::time_point lastFiredAt(Tier t) const;

    const Config& config() const { return cfg_; }

private:
    Config cfg_;

    std::chrono::system_clock::time_point last_fired_ambient_{};
    std::chrono::system_clock::time_point last_fired_active_{};
    std::chrono::system_clock::time_point last_fired_urgent_{};

    std::unordered_map<std::string, int>  consecutive_rejects_;
    struct MuteEntry {
        std::chrono::system_clock::time_point until;
    };
    std::unordered_map<std::string, MuteEntry> muted_;
};

} // namespace arise
