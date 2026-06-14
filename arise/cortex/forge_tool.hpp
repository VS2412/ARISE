#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortex/critic.hpp"
#include "cortex/sandbox.hpp"
#include "cortex/tool_registry.hpp"

namespace arise {

class SubAgent;

// "I want a tool that does X" → end-to-end forge pipeline.
//
//   propose(description, args_schema, example_args, requested_id)
//     1. Sanitise / dedupe id against existing tools.
//     2. Drive Coder with a tool-shaped task ("write {id}.{sh|py} that
//        reads JSON args from stdin and …"). Coder's own Critic gate is
//        the first safety pass; any rejection there short-circuits.
//     3. Locate the entry-point script in the produced bundle (preferring
//        `<id>.sh` then `<id>.py` then any `.sh`/`.py`).
//     4. If `example_args` is given, run the entry point inside a fresh
//        Sandbox profile — fresh `/tmp`, no env, no network — with the
//        sample piped to stdin. Verifies the script at least starts.
//     5. Return a `Proposal` describing what we made + the verdicts. The
//        tool is NOT yet registered.
//
//   stage(proposal, auto_approve)
//     6. Move the bundle into `<root>/tools/learned/<id>/` and register
//        it in the manifest. If `auto_approve` is true (CLI flag), call
//        `ToolRegistry::approve` immediately; otherwise the tool sits
//        unapproved until the user types `arise tools approve <id>`.
//
// The pipeline is intentionally synchronous + handle-free: the forge is
// driven by the user / orchestrator on the foreground thread. Background
// invocation can layer on later via SpawnHandle if needed.
class ForgeTool {
public:
    struct Config {
        SubAgent*       coder_llm = nullptr;        // required
        Critic*         critic     = nullptr;       // required
        ToolRegistry*   registry   = nullptr;       // required
        std::string     sandbox_root;               // staging for Coder + dry-run
        std::string     learned_root;               // <root>/tools/learned/
        int             dry_run_timeout_sec    = 8;
        std::size_t     dry_run_max_stdout     = 16 * 1024;
        std::size_t     dry_run_max_stderr     = 8  * 1024;
        std::size_t     coder_max_files        = 5;
        std::size_t     coder_max_bytes_per_file = 64 * 1024;
        std::chrono::seconds coder_max_wall_time { 90 };
    };

    struct Proposal {
        bool                      ok                 = false;
        bool                      budget_hit         = false;
        bool                      dry_run_skipped    = false;
        std::string               error;

        std::string               id;                // sanitised, ready to register
        std::string               description;
        std::string               summary;           // Coder's one-liner

        std::string               draft_dir;         // <sandbox_root>/coder_<rand>/
        std::string               entry_path;        // absolute path to entrypoint
        std::string               entry_rel;         // relative to draft_dir
        std::string               interpreter;       // "bash" / "python3" / ""
        std::vector<std::string>  files;             // all bundle paths (absolute)

        nlohmann::json            args_schema;
        Critic::Review            critic_review;     // Coder's Critic verdict
        Sandbox::Result           dry_run;           // empty if skipped
    };

    explicit ForgeTool(Config cfg);

    Proposal propose(const std::string& description,
                     const nlohmann::json& args_schema,
                     std::optional<nlohmann::json> example_args = std::nullopt,
                     const std::string& requested_id = "");

    // Move the bundle into <learned_root>/<id>/ and register the tool. If
    // `auto_approve` is true, also flips the approved flag. Returns false on
    // any IO or registry error; the proposal's draft_dir is untouched on
    // failure so the user can inspect.
    bool stage(const Proposal& proposal, bool auto_approve = false);

    const Config& config() const { return cfg_; }

private:
    Config cfg_;

    // Static so tests can exercise without constructing a full ForgeTool.
public:
    // Pick the entry-point script path (relative to bundle dir) given the
    // produced filenames. Prefers `<id>.sh`, then `<id>.py`, then alphabetic
    // `.sh`, then `.py`, then first file. Returns "" if files is empty.
    static std::string pickEntryPoint(const std::vector<std::string>& rel_paths,
                                      const std::string& id);

    // Map a file extension to the interpreter we should hand to Sandbox.
    // ".sh"→bash, ".py"→python3, otherwise "".
    static std::string interpreterFor(const std::string& path);
};

} // namespace arise
