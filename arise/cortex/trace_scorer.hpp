#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

#include "cortex/trace_replay.hpp"

namespace arise {

// LLM-as-judge scorer for one Trace. Phase 9 commit 1.
//
// We ask qwen3:8b (the primary mind) to grade ARISE's response on three
// axes plus an overall. Output is strict JSON; the same parser used by
// SalienceScorer handles markdown / <think> noise.
//
// The scorer is fail-soft: a network blip → `{ok:false, defaults}` so the
// curator can either drop the trace or keep it with `from_llm=false`. We
// never fabricate a high score from a missing LLM.
class TraceScorer {
public:
    struct Score {
        bool         ok               = false;
        bool         from_llm         = false;
        double       tool_correctness = 0.0;
        double       tone_match       = 0.0;
        double       memory_use       = 0.0;
        double       overall          = 0.0;
        std::string  reasoning;
    };

    struct Config {
        std::string  ollama_url       = "http://127.0.0.1:11434";
        std::string  model            = "qwen3:8b";
        int          num_gpu          = 999;       // primary mind: GPU resident
        int          timeout_sec      = 30;
        bool         strip_thinking   = true;
        // Drop the score if `overall` is below this. Curator obeys.
        double       drop_below_overall = 0.4;
    };

    explicit TraceScorer(Config cfg);

    Score score(const Trace& t) const;

    // Pure parser exposed for tests. Pulls a `{tool_correctness,…}` JSON
    // object out of arbitrary LLM text. Strips qwen3 <think> blocks first.
    static std::optional<Score> parse(std::string_view text,
                                      bool strip_thinking = true);

    bool          reachable() const { return reachable_.load(); }
    const Config& config()    const { return cfg_; }

private:
    Config             cfg_;
    mutable std::atomic<bool> reachable_{false};
};

} // namespace arise
