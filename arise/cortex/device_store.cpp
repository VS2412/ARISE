#include "cortex/device_store.hpp"

#include "util/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#include <openssl/sha.h>

#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace std::chrono;

namespace arise {

const char* deviceKindToString(DeviceKind k) {
    switch (k) {
        case DeviceKind::Phone:   return "phone";
        case DeviceKind::Tablet:  return "tablet";
        case DeviceKind::Desktop: return "desktop";
        case DeviceKind::Mqtt:    return "mqtt";
        case DeviceKind::Overlay: return "overlay";
        case DeviceKind::Other:   return "other";
    }
    return "other";
}

std::optional<DeviceKind> deviceKindFromString(std::string_view s) {
    if (s == "phone")   return DeviceKind::Phone;
    if (s == "tablet")  return DeviceKind::Tablet;
    if (s == "desktop") return DeviceKind::Desktop;
    if (s == "mqtt")    return DeviceKind::Mqtt;
    if (s == "overlay") return DeviceKind::Overlay;
    if (s == "other")   return DeviceKind::Other;
    return std::nullopt;
}

namespace {

std::string isoOf(system_clock::time_point t) {
    if (t.time_since_epoch().count() == 0) return {};
    auto tt = system_clock::to_time_t(t);
    std::tm tm_buf{}; gmtime_r(&tt, &tm_buf);
    char buf[32]; std::strftime(buf, sizeof buf, "%FT%TZ", &tm_buf);
    return buf;
}

system_clock::time_point parseIso(const std::string& s) {
    if (s.empty()) return {};
    std::tm tm{};
    if (!strptime(s.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) return {};
    return system_clock::from_time_t(timegm(&tm));
}

bool writeFileAtomic(const std::string& path, const std::string& body) {
    auto tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << body;
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::string makeId(DeviceKind kind) {
    static thread_local std::mt19937_64 rng(
        std::random_device{}() ^ std::uint64_t(
            steady_clock::now().time_since_epoch().count()));
    char buf[12];
    std::snprintf(buf, sizeof buf, "%06lx",
                  (unsigned long)(rng() & 0xFFFFFF));
    return std::string(deviceKindToString(kind)) + "-" + buf;
}

DevicePermissions permsFromJson(const json& j) {
    DevicePermissions p;
    if (!j.is_object()) return p;
    p.can_utterance    = j.value("utterance",    p.can_utterance);
    p.can_decision     = j.value("decision",     p.can_decision);
    p.can_goal_query   = j.value("goal_query",   p.can_goal_query);
    p.can_notification = j.value("notification", p.can_notification);
    p.can_screen_share = j.value("screen_share", p.can_screen_share);
    return p;
}

json permsToJson(const DevicePermissions& p) {
    return json{
        {"utterance",    p.can_utterance},
        {"decision",     p.can_decision},
        {"goal_query",   p.can_goal_query},
        {"notification", p.can_notification},
        {"screen_share", p.can_screen_share},
    };
}

DeviceInfo deviceFromJson(const json& j) {
    DeviceInfo d;
    d.id   = j.value("id", "");
    d.name = j.value("name", "");
    if (auto k = deviceKindFromString(j.value("kind", "other"))) d.kind = *k;
    d.token_sha256_hex = j.value("token_sha256_hex", "");
    if (j.contains("permissions")) d.perms = permsFromJson(j["permissions"]);
    d.paired_at  = parseIso(j.value("paired_at", ""));
    d.last_seen  = parseIso(j.value("last_seen", ""));
    d.event_count = j.value("event_count", std::int64_t(0));
    return d;
}

json deviceToJson(const DeviceInfo& d) {
    json j;
    j["id"]                = d.id;
    j["name"]              = d.name;
    j["kind"]              = deviceKindToString(d.kind);
    j["token_sha256_hex"]  = d.token_sha256_hex;
    j["permissions"]       = permsToJson(d.perms);
    j["paired_at"]         = isoOf(d.paired_at);
    j["last_seen"]         = isoOf(d.last_seen);
    j["event_count"]       = d.event_count;
    return j;
}

} // namespace

// ─── static helpers ────────────────────────────────────────────────────────

std::string DeviceStore::sha256Hex(const std::string& s) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    ::SHA256(reinterpret_cast<const unsigned char*>(s.data()),
             s.size(), digest);
    char buf[3];
    std::string out;
    out.reserve(SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        std::snprintf(buf, sizeof buf, "%02x", digest[i]);
        out.append(buf, 2);
    }
    return out;
}

bool DeviceStore::constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char r = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        r |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return r == 0;
}

std::string DeviceStore::randomTokenHex(int bytes) {
    if (bytes <= 0) return {};
    std::random_device rd;
    std::string out;
    out.reserve(std::size_t(bytes) * 2);
    char buf[3];
    int produced = 0;
    while (produced < bytes) {
        std::uint32_t chunk = rd();
        for (int i = 0; i < 4 && produced < bytes; ++i, ++produced) {
            std::snprintf(buf, sizeof buf, "%02x",
                          unsigned(chunk & 0xFF));
            out.append(buf, 2);
            chunk >>= 8;
        }
    }
    return out;
}

// ─── impl ──────────────────────────────────────────────────────────────────

struct DeviceStore::Impl {
    Config              cfg;
    mutable std::mutex  mu;
    std::vector<DeviceInfo> devices;
};

DeviceStore::DeviceStore(Config cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = std::move(cfg);
    if (p_->cfg.path.empty()) {
        log::error("DeviceStore: empty path");
        return;
    }
    load();
}

DeviceStore::~DeviceStore() = default;

bool DeviceStore::load() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->devices.clear();
    auto blob = slurp(p_->cfg.path);
    if (blob.empty()) return true;       // missing file is fine
    try {
        auto j = json::parse(blob);
        if (j.contains("devices") && j["devices"].is_array()) {
            for (auto& d : j["devices"]) {
                if (d.is_object()) p_->devices.push_back(deviceFromJson(d));
            }
        }
    } catch (const std::exception& e) {
        log::error(std::string("DeviceStore: parse: ") + e.what());
        return false;
    }
    return true;
}

bool DeviceStore::save() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    json j;
    j["version"] = 1;
    j["devices"] = json::array();
    for (const auto& d : p_->devices) j["devices"].push_back(deviceToJson(d));
    return writeFileAtomic(p_->cfg.path, j.dump(2));
}

std::optional<AddDeviceResult>
DeviceStore::addDevice(const std::string& name, DeviceKind kind,
                       DevicePermissions perms) {
    if (name.empty()) return std::nullopt;
    auto plaintext = randomTokenHex(32);
    AddDeviceResult result;
    result.plaintext_token = plaintext;

    DeviceInfo d;
    d.id               = makeId(kind);
    d.name             = name;
    d.kind             = kind;
    d.token_sha256_hex = sha256Hex(plaintext);
    d.perms            = perms;
    d.paired_at        = system_clock::now();

    {
        std::lock_guard<std::mutex> lk(p_->mu);
        // Defensive: if the random id collides, regenerate.
        for (int retry = 0; retry < 5; ++retry) {
            bool clash = false;
            for (const auto& ex : p_->devices) if (ex.id == d.id) { clash = true; break; }
            if (!clash) break;
            d.id = makeId(kind);
        }
        p_->devices.push_back(d);
        result.device = d;
    }
    if (!save()) return std::nullopt;
    return result;
}

std::optional<DeviceInfo> DeviceStore::getById(const std::string& id) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const auto& d : p_->devices) if (d.id == id) return d;
    return std::nullopt;
}

std::optional<DeviceInfo>
DeviceStore::getByToken(const std::string& plaintext_token) const {
    if (plaintext_token.empty()) return std::nullopt;
    auto presented_hex = sha256Hex(plaintext_token);
    std::lock_guard<std::mutex> lk(p_->mu);
    // Walk every device with constant-time compare to avoid timing leaks.
    std::optional<DeviceInfo> hit;
    for (const auto& d : p_->devices) {
        // If two stored tokens accidentally have different hashes, the
        // constant-time compare on the first one runs to completion before
        // we look at the next; that's intentional.
        if (constantTimeEquals(d.token_sha256_hex, presented_hex)) {
            hit = d;          // don't break — keep walking for uniform timing
        }
    }
    return hit;
}

bool DeviceStore::recordSeen(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& d : p_->devices) {
            if (d.id == id) {
                d.last_seen   = system_clock::now();
                d.event_count = d.event_count + 1;
                goto saved;
            }
        }
        return false;
    }
saved:
    return save();
}

bool DeviceStore::revokeById(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        auto it = std::find_if(p_->devices.begin(), p_->devices.end(),
                               [&](const DeviceInfo& d){ return d.id == id; });
        if (it == p_->devices.end()) return false;
        p_->devices.erase(it);
    }
    return save();
}

std::vector<DeviceInfo> DeviceStore::list() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->devices;
}

} // namespace arise
