#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cortex/feedback_db.hpp"
#include "cortex/suggestion.hpp"

namespace arise {

class Blackboard;
class MemoryCortex;

// Phase 6 commit 1 — proactive engine. Subscribes wildcard, filters in its
// own thread, deterministically maps already-judged blackboard signals onto
// `Suggestion`s, runs them through `SuggestionGate`, persists via
// `FeedbackDb`, and republishes as `proactive.suggestion` events. The LLM-
// judged path (caption-with-error → "want help on line 47?") and calendar
// integration land in commit 2.
//
// Topics consumed:
//   * agent.watcher.notice — already tier-classified by the Watcher; we
//     just translate severity → Tier and forward.
//   * goal.due / goal.escalated / goal.stale — concrete actionable nudges.
//   * audio.scene_changed — alarm/doorbell/phone become Urgent/Active.
//
// Topics published:
//   * proactive.suggestion — the gate-passed text the user-facing layer
//     should TTS / display. Payload includes id (for `arise proactive
//     decide`), tier, category, text, source_topic.
//   * proactive.dropped    — when a candidate suggestion is built but the
//     gate blocks it (rate limit / quiet hours / muted). Useful for the
//     monitor page — never spoken.
class ProactiveEngine {
public:
    struct Config {
        Blackboard*    bb        = nullptr;     // required
        FeedbackDb*    feedback  = nullptr;     // required
        MemoryCortex*  cortex    = nullptr;     // optional — episodic mirror
        SuggestionGate::Config gate;            // tier rate limits + quiet hours

        // When true, the engine emits `proactive.dropped` events for blocked
        // suggestions in addition to silently dropping them. Off by default
        // so non-listeners aren't flooded.
        bool publish_dropped = false;
    };

    struct Stats {
        std::size_t signals_seen      = 0;
        std::size_t suggested         = 0;     // built (before gate)
        std::size_t passed            = 0;     // emitted to bus + persisted
        std::size_t blocked_rate      = 0;
        std::size_t blocked_quiet     = 0;
        std::size_t blocked_muted     = 0;
        std::size_t blocked_silent    = 0;
    };

    explicit ProactiveEngine(Config cfg);
    ~ProactiveEngine();
    ProactiveEngine(const ProactiveEngine&)            = delete;
    ProactiveEngine& operator=(const ProactiveEngine&) = delete;

    void  start();
    void  stop();
    bool  running() const;

    Stats stats() const;

    // For tests / CLI: synchronous evaluator. Builds a candidate suggestion
    // from a blackboard event payload, optionally runs it through the gate,
    // and (if pass) persists + publishes. Returns the (possibly-Pending)
    // suggestion. Empty result.id == "didn't build any suggestion".
    struct EvalResult {
        bool                published = false;
        Suggestion          suggestion;
        GateOutcome         outcome = GateOutcome::Pass;
    };
    EvalResult evaluate(const std::string& topic,
                        const nlohmann::json& payload);

    // Manually mute / unmute a category from the CLI. Persists nothing —
    // mute state lives in the gate's in-memory map and resets when the
    // engine stops. Long-term mute would lean on FeedbackDb counters.
    void muteCategory(const std::string& category,
                      std::chrono::seconds duration);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;

    void workerLoop_();

    // Pure mappers from a blackboard event → candidate suggestion. Returns
    // {tier=Silent} when the topic isn't actionable.
    static Suggestion buildFromWatcherNotice(const nlohmann::json& payload,
                                             const std::string& topic);
    static Suggestion buildFromGoalEvent(const nlohmann::json& payload,
                                         const std::string& topic);
    static Suggestion buildFromAudioScene(const nlohmann::json& payload,
                                          const std::string& topic);

public:
    // Static so tests can probe the mapping table without spinning up a
    // full engine.
    static Suggestion buildCandidate(const std::string& topic,
                                     const nlohmann::json& payload);
};

} // namespace arise
