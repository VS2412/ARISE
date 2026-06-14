#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cortex/feedback_db.hpp"
#include "cortex/memory_cortex.hpp"

namespace arise {

// One reconstructed (user_input, ARISE_response, outcome) tuple. The
// `outcome` lives separately because it usually arrives later (or never).
// `source_event_ids` carries every episodic id that contributed so the
// curator can drop the whole bundle if any of them tripped the privacy
// filter.
struct Trace {
    std::int64_t                              id = 0;          // = user_event_id
    std::string                               source_device;   // "" when desktop
    std::string                               user_input;
    std::string                               response_kind;   // proactive_suggestion / goal_state / ...
    std::string                               response_summary;
    std::string                               response_payload_json;
    std::optional<std::int64_t>               suggestion_id;
    std::optional<Decision>                   outcome;
    Timestamp                             user_at;
    std::optional<Timestamp>              response_at;
    std::optional<Timestamp>              outcome_at;
};

class TraceReplay {
public:
    struct Config {
        // How far back to look. The replayer pulls min(needed, max_events)
        // recent rows out of the cortex and filters in-process.
        std::chrono::hours    lookback         { 24 };
        std::chrono::seconds  response_window  { 60 };
        // How many user_input events to scan, max.
        int                   max_events       = 5000;
        // Episodic kinds that count as "user input". The default lines up
        // with the federation channel (`federation_utterance`), plus older
        // ARIA-imported `conversation_turn` rows when present.
        std::vector<std::string> user_kinds = {
            "federation_utterance", "conversation_turn",
        };
        // Episodic kinds that count as "response". First match within the
        // window after a user input is paired up.
        std::vector<std::string> response_kinds = {
            "proactive_suggestion", "goal_state", "watcher_notice",
        };
    };

    explicit TraceReplay(Config cfg);

    // Walks `cortex.recentEvents(max_events)` once, pulls `feedback.list()`
    // once, returns a chronologically-ordered vector of Traces. Pure read —
    // no mutation.
    std::vector<Trace> replay(const MemoryCortex& cortex,
                              const FeedbackDb&  feedback) const;

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

} // namespace arise
