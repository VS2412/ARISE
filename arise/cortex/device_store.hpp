#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace arise {

// What kind of shard is on the other end of the wire.
enum class DeviceKind {
    Phone,
    Tablet,
    Desktop,         // a peer ARISE
    Mqtt,            // home automation bridge
    Overlay,         // Wayland layer-shell second screen
    Other,
};

const char*                   deviceKindToString(DeviceKind k);
std::optional<DeviceKind>     deviceKindFromString(std::string_view s);

// What this device is allowed to do. Permissions are checked per inbound
// event before the FederationRouter dispatches a handler.
struct DevicePermissions {
    bool can_utterance      = true;   // shard can submit user speech
    bool can_decision       = true;   // shard can flip suggestion decisions
    bool can_goal_query     = true;   // shard can ask "what's pending?"
    bool can_notification   = false;  // can the shard inject a notification (rare)
    bool can_screen_share   = false;  // future
};

// On-disk device record. The plaintext token is *never* stored — only its
// hex SHA-256 hash. addDevice() is the only function that ever touches the
// plaintext, returning it once to the caller; lose it and the device must
// re-pair.
struct DeviceInfo {
    std::string                                id;          // "<kind>-<6hex>"
    std::string                                name;        // human label
    DeviceKind                                 kind = DeviceKind::Phone;
    std::string                                token_sha256_hex;  // 64 chars
    DevicePermissions                          perms;
    std::chrono::system_clock::time_point      paired_at{};
    std::chrono::system_clock::time_point      last_seen{};
    std::int64_t                               event_count = 0;
};

struct AddDeviceResult {
    DeviceInfo  device;          // stored copy (no plaintext)
    std::string plaintext_token; // 64-char hex; print once, then forget
};

// Disk-backed device registry under `<root>/devices.json`. Thread-safe
// (one mutex around the in-memory list). Atomic save (`*.tmp` + rename).
class DeviceStore {
public:
    struct Config {
        std::string path;        // <root>/devices.json
    };

    explicit DeviceStore(Config cfg);
    ~DeviceStore();
    DeviceStore(const DeviceStore&)            = delete;
    DeviceStore& operator=(const DeviceStore&) = delete;

    bool load();
    bool save() const;

    // Generate a fresh 32-byte hex token, hash it, persist a new device
    // record, and return the *one-time-visible* plaintext token. Callers
    // must show the token to the user immediately; it is gone forever
    // afterwards.
    std::optional<AddDeviceResult> addDevice(
        const std::string& name,
        DeviceKind kind = DeviceKind::Phone,
        DevicePermissions perms = DevicePermissions{});

    // Look up by id; no token needed (used by `arise federation list`).
    std::optional<DeviceInfo> getById(const std::string& id) const;

    // Authenticate a presented plaintext token. Returns the matching
    // device or nullopt. Constant-time across all stored devices.
    std::optional<DeviceInfo> getByToken(const std::string& plaintext_token) const;

    // Bump last_seen + event_count. Always saves.
    bool recordSeen(const std::string& id);

    bool revokeById(const std::string& id);

    std::vector<DeviceInfo> list() const;

    // Helpers exposed for tests + the router.
    static std::string sha256Hex(const std::string& s);
    static bool        constantTimeEquals(const std::string& a,
                                          const std::string& b);
    static std::string randomTokenHex(int bytes = 32);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace arise
