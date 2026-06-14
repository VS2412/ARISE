#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "cortex/critic.hpp"

namespace arise {

class SubAgent;

// Lightweight code-generation sub-agent. Phase 4 commit 2 ships the safe
// path: the LLM emits a JSON file plan, the Coder writes the files into a
// fresh sandbox directory, the Critic reviews the concatenated content,
// and we hand the user the diff (filenames + sizes + Critic verdict). No
// auto-execution. No git worktree witchcraft yet — that lives in Phase 5
// (Tool Forge) where the bwrap sandbox is also built out.
class Coder {
public:
    struct Config {
        SubAgent*     llm = nullptr;       // required
        Critic*       critic = nullptr;    // required — gates the result
        std::string   sandbox_root;        // <root>/sandbox/coder/<id>/
        int           max_files       = 5;
        int           max_bytes_per_file = 64 * 1024;
        std::chrono::seconds  max_wall_time { 90 };
    };

    struct WrittenFile {
        std::string path;        // absolute path inside sandbox
        std::string rel_path;    // relative to sandbox_root
        std::size_t bytes = 0;
    };

    struct Result {
        bool                       ok          = false;
        bool                       budget_hit  = false;
        bool                       killed      = false;
        std::string                error;
        std::string                sandbox_path;
        std::vector<WrittenFile>   files;
        Critic::Review             review;       // Critic verdict on the bundle
        std::string                summary;      // LLM's own one-line description
    };

    explicit Coder(Config cfg);

    // Synchronous run. `kill` checked before each LLM call + file write.
    Result run(const std::string& task,
               std::atomic<bool>* kill = nullptr);

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

} // namespace arise
