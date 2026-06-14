#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

namespace arise {

// What a salience scorer returns. Defaults to a "skip" signal so a parser
// failure on the LLM side never accidentally promotes a noise event into
// permanent memory.
struct SalienceScore {
    double      salience = 0.3;   // 0..1; 0 means "definitely don't keep"
    std::string reason;           // optional, captured for episodic.payload
    bool        from_llm = false; // false = fell back to default_score
};

// Tiny LLM that scores "is this worth remembering" on a 0..1 scale. Default
// model is qwen3:0.6b (smallest qwen3 currently published in Ollama; the plan
// originally targeted 0.5b which doesn't exist as a tag — same family,
// negligible cost difference). num_gpu=0 keeps it on CPU so the primary mind
// can stay GPU-resident.
//
// Output format: the model is asked for strict JSON
//   {"salience": <0..1>, "reason": "<one phrase>"}
// Any noise around it (markdown fences, qwen3 <think> blocks, prose) is
// stripped before parsing. On any failure we return default_score with
// from_llm=false so callers can downstream-decide whether to even write.
class SalienceScorer {
public:
    struct Config {
        std::string ollama_url          = "http://127.0.0.1:11434";
        std::string model               = "qwen3:0.6b";
        int         num_gpu             = 0;
        std::string keep_alive          = "30m";
        int         timeout_sec         = 30;
        int         connect_timeout_sec = 2;
        double      default_score       = 0.3;   // when LLM unavailable / unparseable
        bool        strip_thinking      = true;  // qwen3 emits <think>...</think>
    };

    explicit SalienceScorer(Config cfg);

    // Score an arbitrary observation. `kind` (e.g. "screen_obs",
    // "conversation_turn") is included in the prompt so the rubric can lean
    // appropriately. Always returns a value — never throws.
    SalienceScore score(const std::string& kind, const std::string& summary);

    // Static parser exposed for tests. Pulls the first JSON object out of
    // arbitrary text, optionally strips <think>...</think>.
    static std::optional<SalienceScore> parse(std::string_view text,
                                              bool strip_thinking = true);

    bool reachable() const { return reachable_.load(); }

    const Config& config() const { return cfg_; }

private:
    Config             cfg_;
    std::atomic<bool>  reachable_{false};

    std::string callOllama_(const std::string& prompt);
};

} // namespace arise
