#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace arise {

// One LoRA / adapter version on disk. Tracking is content-light: we store
// metadata about the adapter (path, eval score, base model). Phase 9
// commit 2 hands the trainer a path under `<root>/adapters/` and we
// register it here with the rolling eval result.
struct AdapterInfo {
    std::string                            id;          // human label or hash
    std::string                            base_model;  // e.g. "qwen3:8b"
    std::string                            path;        // absolute path to .safetensors
    double                                 eval_score = 0.0;     // composite 0..1
    double                                 baseline_score = 0.0; // score it had to beat
    std::chrono::system_clock::time_point  created_at;
    std::chrono::system_clock::time_point  evaluated_at;
    bool                                   deployed = false;
    bool                                   rolled_back = false;
    std::string                            note;        // optional human note
};

// JSON-on-disk registry of every adapter we've trained, evaluated, or
// rolled back. Path is `<root>/adapters/manifest.json`.
class AdapterRegistry {
public:
    struct Config {
        std::string manifest_path;          // <root>/adapters/manifest.json
        // Cap retained adapters. After registering a new one, the registry
        // archives (still on disk, flagged) all but the most recent N.
        int         keep = 5;
    };

    explicit AdapterRegistry(Config cfg);
    ~AdapterRegistry();
    AdapterRegistry(const AdapterRegistry&)            = delete;
    AdapterRegistry& operator=(const AdapterRegistry&) = delete;

    bool                       load();
    bool                       save() const;

    // Register a freshly-trained adapter. Sets created_at to now. Replaces
    // any existing row with the same `id`. Saves on success.
    bool                       registerAdapter(AdapterInfo info);

    // Mark `id` deployed (and clear `deployed` on every other row). Returns
    // false if `id` is missing.
    bool                       promote(const std::string& id);

    // Roll back: clears `deployed`, sets `rolled_back=true`. Caller is
    // responsible for promoting whatever they want as the new active row.
    bool                       rollback(const std::string& id);

    // Update the eval score / baseline + evaluated_at on an existing row.
    bool                       recordEval(const std::string& id,
                                          double eval_score,
                                          double baseline_score,
                                          std::string note = "");

    // Remove all but the most recent `keep` rows (sorted by created_at
    // descending). Currently-deployed rows are never removed regardless.
    int                        rotate();

    std::optional<AdapterInfo> get(const std::string& id) const;
    std::optional<AdapterInfo> deployed() const;
    std::vector<AdapterInfo>   list() const;

    // CLI helpers.
    std::string path() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace arise
