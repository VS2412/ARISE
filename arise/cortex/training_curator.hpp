#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cortex/privacy_filter.hpp"
#include "cortex/trace_replay.hpp"
#include "cortex/trace_scorer.hpp"

namespace arise {

class MemoryCortex;
class FeedbackDb;

// One Trace that has cleared every gate and is ready to be written into a
// training JSONL.
struct TrainingExample {
    Trace                 trace;
    TraceScorer::Score    score;
    // Mirror of the trace fields after privacy redaction. JSONL writers
    // should use these, not the raw trace.
    std::string           sanitised_user_input;
    std::string           sanitised_response;
};

// Replay → score → privacy-filter → top-N orchestrator. Pure data plane —
// no LoRA training in commit 1.
class TrainingCurator {
public:
    struct Config {
        TraceReplay::Config replay;
        TraceScorer::Config scorer;
        PrivacyFilter::Config privacy;
        // Final cap on how many examples to keep per run. Anything below
        // `scorer.drop_below_overall` after scoring is dropped first.
        int                 top_n = 200;
        // When set, scoring is skipped and every replayed Trace passes
        // through with score.from_llm=false. Useful for offline curation.
        bool                skip_llm_scoring = false;
    };

    struct Stats {
        std::size_t replayed_total       = 0;
        std::size_t scored               = 0;
        std::size_t dropped_low_score    = 0;
        std::size_t dropped_privacy      = 0;
        std::size_t dropped_empty        = 0;
        std::size_t kept                 = 0;
    };

    explicit TrainingCurator(Config cfg);

    std::vector<TrainingExample>
    curate(const MemoryCortex& cortex, const FeedbackDb& feedback,
           Stats* out_stats = nullptr) const;

    // Write `examples` as one-JSON-object-per-line UTF-8. Returns the path
    // written (under `<root>/training/`) or empty on failure.
    static std::string writeJsonl(const std::vector<TrainingExample>& examples,
                                  const std::string& dir);

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

} // namespace arise
