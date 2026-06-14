#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace arise {

// One callable tool. Either a hardcoded "builtin" (id is in the builtin
// table, the registry doesn't read or write a manifest entry for it) or a
// "learned" tool stored under `<root>/tools/learned/`.
//
// args_schema is a tiny subset of JSON Schema: object type with `properties`
// and optional `required`. `properties[k].type` is one of `string`,
// `integer`, `number`, `boolean`. That's all we need for Phase 5; richer
// schema support can come later if Coder / Forge ever need it.
struct ToolInfo {
    std::string                  id;
    int                          version = 1;
    std::string                  description;

    // Execution surface.
    std::string                  interpreter;        // "bash", "python3", "" for builtins
    std::string                  script_path;        // absolute path on disk for learned
    nlohmann::json               args_schema;        // {type:object, properties:{...}, required:[...]}

    // Permissions
    bool                         allow_network = false;
    std::vector<std::string>     writable_paths;

    // Approval / lifecycle.
    bool                         approved = false;
    std::string                  approved_at;
    std::string                  approved_by;       // "user" / "auto" / etc.

    // Usage tracking.
    int                          usage_count = 0;
    std::string                  last_used;
    std::string                  created_at;

    // True for compiled-in tools surfaced by ToolRegistry::listAll().
    bool                         is_builtin = false;
};

// Registry of callable tools — both compiled-in builtins and forged
// "learned" tools loaded from `<root>/tools/learned/manifest.json`.
//
// Phase 5 commit 1 ships:
//   * load() / save()    — round-trip the learned manifest
//   * addLearned()       — register a forged tool
//   * approve / archive / remove
//   * recordUse()        — bump counters
//   * validateArgs()     — minimal schema check (type + required)
//   * builtinTable()     — hardcoded catalog the LLM can also call
//
// Forge integration (Coder → Critic → Sandbox dry-run → user approval) lives
// in Phase 5 commit 2.
class ToolRegistry {
public:
    struct Config {
        std::string root;       // <root>/tools (without trailing slash)
    };

    explicit ToolRegistry(Config cfg);
    ~ToolRegistry();
    ToolRegistry(const ToolRegistry&)            = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;

    // Load manifest from disk. Idempotent. Missing manifest is treated as
    // "no learned tools yet" — not an error.
    bool                  load();

    // Save the learned manifest atomically (write tmp + rename).
    bool                  save() const;

    std::vector<ToolInfo> listAll(bool include_builtins = true) const;
    std::vector<ToolInfo> listLearned() const;
    std::optional<ToolInfo> get(const std::string& id) const;

    // Tool authoring.
    bool                  addLearned(ToolInfo tool);    // saves
    bool                  approve(const std::string& id, const std::string& by = "user");
    bool                  archive(const std::string& id);    // moves manifest entry to archived
    bool                  remove (const std::string& id);    // removes manifest + script file
    bool                  recordUse(const std::string& id);  // bumps counter

    // Sweep through learned tools and archive any whose `last_used` (or
    // `created_at` if never used) is older than `older_than_days` and
    // approval is in place. Returns the count archived. With dry_run=true
    // returns the count that *would* be archived without changing anything.
    int                   sweepStale(int older_than_days, bool dry_run = false);

    // Returns "" on success, or a human-readable error.
    std::string           validateArgs(const std::string& id,
                                       const nlohmann::json& args) const;

    // Sanitise a requested id: lowercase, replace non-[a-z0-9_] with '_',
    // collapse runs, trim. If the result is empty or matches a builtin or
    // an existing learned id, fall back to `<base>_<random hex>`. Never
    // returns an id that begins with "builtin:".
    std::string           sanitiseToolId(const std::string& requested) const;

    // Built-in tool catalog. Static so callers don't need a registry to
    // enumerate them. Ids prefixed with "builtin:" so they never collide
    // with learned ids.
    static const std::vector<ToolInfo>& builtinTable();

    // Helpers
    std::string learnedDir() const;
    std::string archivedDir() const;
    std::string sandboxDir() const;
    std::string manifestPath() const;
    std::string archivedManifestPath() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Validates `args` against `schema` (the small JSON-Schema subset we honor).
// Returns "" on success or a human-readable error.
std::string validateArgsAgainstSchema(const nlohmann::json& schema,
                                      const nlohmann::json& args);

} // namespace arise
