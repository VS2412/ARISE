#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace arise {

// Pause vision when a privacy-sensitive app (banking, password manager, etc.)
// is focused. The probe is pluggable so tests don't need a live niri session.
//
// Failsafe behaviour: if `private_apps` is empty, always allow. If it's
// non-empty AND the probe returns no info, behaviour follows
// `failsafe_private_on_probe_error` — default is *open* (allow vision) so
// missing niri doesn't break perception in dev. Flip it on for prod-grade
// privacy where missing info should mean "block".
class PrivacyGate {
public:
    using Probe = std::function<std::string()>;     // returns active app id

    struct Config {
        std::vector<std::string>  private_apps;
        std::chrono::milliseconds cache_ttl = std::chrono::milliseconds(5000);
        bool failsafe_private_on_probe_error = false;
    };

    explicit PrivacyGate(Config cfg);

    // True if vision should be paused right now. Cached; refreshes after ttl.
    bool isPrivate();

    // Last matched private app id (or empty on the most recent allowed check).
    std::string lastMatched() const;

    // Test seam: replace the default niri probe with a stub.
    void setProbe(Probe p);

    // Pure helper — does not consult cache or run a probe. Useful for unit tests.
    bool wouldBlock(const std::string& app_id) const;

    const std::vector<std::string>& privateApps() const { return cfg_.private_apps; }

private:
    Config                                 cfg_;
    Probe                                  probe_;
    mutable std::mutex                     mu_;
    std::chrono::steady_clock::time_point  last_check_{};
    bool                                   last_private_ = false;
    std::string                            last_match_;
};

} // namespace arise
