#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace arise {

// Generic single-call sub-agent over Ollama's /api/generate.
//
// Phase 4 commit 1 — every sub-agent (Watcher, Curator, later Researcher /
// Coder / Critic) wraps one of these. The contract is intentionally small:
// you give a task string, you get back a `Result` with the LLM's output, the
// time it took, and whether the budget tripped. Tool-calling, ReAct loops,
// and worktree isolation arrive in commit 2.
//
// Pinned to CPU by default (`num_gpu=0`) so the primary mind stays GPU
// resident. Default model is `qwen3:0.6b` — same family the salience scorer
// uses, so loading it once warms it for everything.
class SubAgent {
public:
    struct Config {
        std::string role            = "subagent";    // logging tag
        std::string model           = "qwen3:0.6b";
        std::string ollama_url      = "http://127.0.0.1:11434";
        std::string keep_alive      = "30m";
        int         num_gpu         = 0;            // 0 = CPU pinned
        int         timeout_sec     = 30;            // hard wall-clock budget
        int         connect_timeout_sec = 2;
        int         max_predict     = 512;          // soft token budget
        bool        format_json     = false;        // ask Ollama for JSON-only
        bool        strip_thinking  = true;         // strip qwen3 <think>...
        std::string system_prompt;                  // prepended to every call
        double      temperature     = 0.0;          // deterministic by default
    };

    struct Result {
        bool                  ok           = false;
        bool                  budget_hit   = false; // wall-clock or token cap
        bool                  reachable    = false; // ollama answered at all
        std::string           output;               // post-strip caller-facing text
        std::string           raw;                  // pre-strip, for diag
        int                   duration_ms  = 0;
        std::string           error;
        std::optional<nlohmann::json> json; // populated on format_json=true + parseable
    };

    explicit SubAgent(Config cfg);

    // Synchronous. Bounded by `timeout_sec`. Safe to call concurrently from
    // multiple threads; each call gets its own curl handle.
    Result run(std::string_view task) const;

    const Config& config() const { return cfg_; }

    // Strip <think>...</think> blocks (qwen3 reasoning prefix). Greedy + nested-safe.
    static std::string stripThinkingBlocks(std::string_view in);

    // Find the first balanced top-level JSON object in arbitrary text.
    // Tolerant of markdown fences and trailing prose.
    static std::optional<std::string> firstJsonObject(std::string_view s);

private:
    Config            cfg_;
    mutable std::atomic<bool> reachable_{false};

    std::string callOllama_(const std::string& prompt, int& out_duration_ms,
                            std::string& out_error) const;
};

} // namespace arise
