#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace arise {

class SubAgent;

// ReAct-style research sub-agent: alternating LLM `thought / action` steps
// with sandboxed tool calls. Phase 4 commit 2 ships three actions:
//
//   * read_file(path)   — reads up to N bytes; path must resolve under
//                         `sandbox_root` (no `..` escapes, no symlink
//                         traversal beyond it)
//   * list_dir(path)    — returns file + dir names; same path discipline
//   * final(answer)     — terminates the loop with the answer
//
// Anything else the model emits (`http_get`, `search_web`, …) is rejected
// with a structured observation so the model can self-correct or just hit
// the budget. Network access is intentionally out of scope until Phase 5
// when the Tool Forge can vet specific URL prefixes.
class Researcher {
public:
    struct Config {
        SubAgent*     llm = nullptr;       // required
        std::string   sandbox_root;        // restricts read_file / list_dir
        int           max_tool_calls = 8;
        std::chrono::seconds  max_wall_time { 60 };
        int           max_file_bytes  = 16 * 1024;
        int           max_listing     = 200;
    };

    struct Step {
        std::string     thought;
        std::string     action;
        nlohmann::json  args;
        std::string     observation;       // result of running the tool
    };

    struct Result {
        bool                ok          = false;
        std::string         answer;
        std::vector<Step>   steps;
        bool                budget_hit  = false;
        bool                killed      = false;
        std::string         error;
    };

    explicit Researcher(Config cfg);

    // Synchronous run. `kill` is consulted between LLM round-trips so the
    // outer Spawn handle can ask for an early bail-out.
    Result run(const std::string& task,
               std::atomic<bool>* kill = nullptr);

    const Config& config() const { return cfg_; }

private:
    Config cfg_;

    std::string runReadFile_(const std::string& rel_path) const;
    std::string runListDir_ (const std::string& rel_path) const;

    // Path-canonicaliser: refuses anything that resolves outside sandbox_root.
    // Returns empty string on rejection.
    std::string resolveSandboxPath_(const std::string& rel_path) const;
};

} // namespace arise
