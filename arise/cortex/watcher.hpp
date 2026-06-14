#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "cortex/sub_agent.hpp"

namespace arise {

class Blackboard;
class GoalStore;
class MemoryCortex;

// "What was that?" agent. Subscribes to perception + system signals on the
// blackboard and decides — usually with a fast-path rule, sometimes with a
// brief LLM judgment — whether to publish a notice, propose a goal, or
// stay silent.
//
// Phase 4 commit 1 covers the rule-based core paths:
//   * battery_pct dropping below warning / critical thresholds
//   * vision.caption matching an `error|exception|fail|...` pattern
//   * audio.scene_changed → speech / phone / doorbell / alarm
//
// Each handler emits an `agent.watcher.notice` event. Goal proposal is
// triggered only by truly user-actionable signals (battery_critical,
// alarm) so the goal stack stays meaningful.
class Watcher {
public:
    enum class Severity {
        Silent,   // log only
        Ambient,  // one-line, low-priority notice
        Active,   // user-actionable; suggested goal exists
        Urgent,   // speak even if user is busy (battery 5%, security alert)
    };

    static const char* severityToString(Severity s);

    struct Config {
        Blackboard*    bb     = nullptr;       // required
        GoalStore*     goals  = nullptr;       // optional — auto-propose goals
        MemoryCortex*  cortex = nullptr;       // optional — episodic mirror
        SubAgent*      llm    = nullptr;       // optional — for caption-error nuance

        // Battery thresholds (percent).
        int battery_warning_pct  = 25;
        int battery_critical_pct = 10;

        // vision.caption keyword fast-path. Lowercased substring match.
        std::vector<std::string> error_keywords = {
            "error", "exception", "failed", "failure", "crashed", "traceback",
            "segfault", "panic", "denied", "unauthorized", "404",
        };

        // Behaviour switches.
        bool propose_goals_on_critical = true;
        bool publish_silent_notices    = false;  // false = drop "ignore" notices
    };

    explicit Watcher(Config cfg);
    ~Watcher();
    Watcher(const Watcher&)            = delete;
    Watcher& operator=(const Watcher&) = delete;

    void start();
    void stop();
    bool running() const;

    struct Stats {
        std::size_t events_seen      = 0;
        std::size_t notices_emitted  = 0;
        std::size_t goals_proposed   = 0;
        std::size_t llm_consultations = 0;
    };
    Stats stats() const;

    // For tests / CLI: synchronous handlers used by `arise watcher fire`
    // and the unit tests. They build the same payload the worker would and
    // either publish + propose like real, or just compute the decision.
    struct Decision {
        Severity     severity   = Severity::Silent;
        std::string  kind;          // e.g. "battery_critical"
        std::string  summary;
        bool         propose_goal = false;
        std::string  goal_summary;
        int          goal_priority = 50;
    };

    // Pure: no side effects. Used to test the rules.
    Decision evaluateBatteryPct(int pct) const;
    Decision evaluateCaption  (const std::string& caption) const;
    Decision evaluateAudioScene(const std::string& scene_label) const;

    // Side-effecting: applies the decision (publishes, optionally proposes,
    // optionally writes episodic). Returns the decision used.
    Decision applyDecision(Decision d, const std::string& source_topic);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;

    void workerLoop_();
};

} // namespace arise
