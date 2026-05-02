#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace arise::sys {

// "What is true about the machine right now." Every field is optional so a
// missing tool / sensor doesn't poison the rest of the snapshot.
struct Snapshot {
    std::optional<std::string> active_app;       // niri app_id of focused window
    std::optional<std::string> active_title;     // title of focused window
    std::optional<int>         workspace_id;     // niri workspace id

    std::optional<int>         volume_pct;       // 0..100
    std::optional<bool>        muted;

    std::optional<int>         brightness_pct;   // 0..100

    std::optional<int>         battery_pct;      // 0..100
    std::optional<std::string> battery_status;   // "Charging" / "Discharging" / "Full"

    std::optional<std::string> network_name;     // SSID / "wired" / "" if disconnected
};

// Snapshot the live system state by shelling out to niri / wpctl /
// brightnessctl / sysfs. ~20-50ms total. Missing tools just leave fields empty.
Snapshot take();

// JSON of fields that differ between prev and curr (current values only).
// Empty object when prev == curr. Used as the payload of `system.delta`.
nlohmann::json delta(const Snapshot& prev, const Snapshot& curr);

// Full snapshot serialised. Used as the payload of `system.snapshot`.
nlohmann::json toJson(const Snapshot& s);

} // namespace arise::sys
