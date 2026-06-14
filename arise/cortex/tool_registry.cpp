#include "cortex/tool_registry.hpp"

#include "util/log.hpp"
#include "util/paths.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

std::string nowIso() {
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof buf, "%FT%TZ", &tm_buf);
    return buf;
}

// Convert a ToolInfo ↔ JSON. The on-disk shape is verbose but explicit so a
// human auditing `learned/manifest.json` can read it without context.
json toJson(const ToolInfo& t) {
    json j;
    j["id"]              = t.id;
    j["version"]         = t.version;
    j["description"]     = t.description;
    j["interpreter"]     = t.interpreter;
    j["script_path"]     = t.script_path;
    j["args_schema"]     = t.args_schema;
    j["allow_network"]   = t.allow_network;
    j["writable_paths"]  = t.writable_paths;
    j["approved"]        = t.approved;
    j["approved_at"]     = t.approved_at;
    j["approved_by"]     = t.approved_by;
    j["usage_count"]     = t.usage_count;
    j["last_used"]       = t.last_used;
    j["created_at"]      = t.created_at;
    return j;
}

ToolInfo fromJson(const json& j) {
    ToolInfo t;
    t.id              = j.value("id", "");
    t.version         = j.value("version", 1);
    t.description     = j.value("description", "");
    t.interpreter     = j.value("interpreter", "");
    t.script_path     = j.value("script_path", "");
    if (j.contains("args_schema") && j["args_schema"].is_object())
        t.args_schema = j["args_schema"];
    else
        t.args_schema = json::object();
    t.allow_network   = j.value("allow_network", false);
    if (j.contains("writable_paths") && j["writable_paths"].is_array())
        t.writable_paths = j["writable_paths"].get<std::vector<std::string>>();
    t.approved        = j.value("approved", false);
    t.approved_at     = j.value("approved_at", "");
    t.approved_by     = j.value("approved_by", "");
    t.usage_count     = j.value("usage_count", 0);
    t.last_used       = j.value("last_used", "");
    t.created_at      = j.value("created_at", "");
    return t;
}

bool writeFileAtomic(const std::string& path, const std::string& body) {
    auto tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << body;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

} // namespace

// ─── builtin catalog ───────────────────────────────────────────────────────

const std::vector<ToolInfo>& ToolRegistry::builtinTable() {
    static const std::vector<ToolInfo> v = []{
        std::vector<ToolInfo> out;

        auto mk = [&](std::string id, std::string desc,
                      json schema, bool net = false) {
            ToolInfo t;
            t.id              = "builtin:" + std::move(id);
            t.version         = 1;
            t.description     = std::move(desc);
            t.args_schema     = std::move(schema);
            t.allow_network   = net;
            t.approved        = true;
            t.approved_by     = "compile-time";
            t.is_builtin      = true;
            return t;
        };

        // Memory cortex bridges (the LLM-facing surface — actual handlers
        // live elsewhere; the catalog just declares the tool exists).
        out.push_back(mk("mem_record",
            "Record an episodic memory event.",
            json{
                {"type", "object"},
                {"properties", {
                    {"summary",  {{"type","string"}}},
                    {"kind",     {{"type","string"}}},
                    {"salience", {{"type","number"}}},
                }},
                {"required", {"summary"}},
            }));
        out.push_back(mk("mem_recall",
            "Hybrid recall over episodic + semantic memory.",
            json{
                {"type", "object"},
                {"properties", {
                    {"query", {{"type","string"}}},
                    {"limit", {{"type","integer"}}},
                }},
                {"required", {"query"}},
            }));
        out.push_back(mk("goal_propose",
            "Propose a new goal.",
            json{
                {"type", "object"},
                {"properties", {
                    {"summary",  {{"type","string"}}},
                    {"priority", {{"type","integer"}}},
                    {"deadline", {{"type","string"}}},
                }},
                {"required", {"summary"}},
            }));
        out.push_back(mk("goal_complete",
            "Mark a goal done.",
            json{
                {"type", "object"},
                {"properties", { {"id", {{"type","integer"}}} }},
                {"required", {"id"}},
            }));
        out.push_back(mk("read_screen",
            "Capture + caption the current screen.",
            json{
                {"type", "object"},
                {"properties", {{"prompt", {{"type","string"}}}}},
            }));
        out.push_back(mk("forge_tool",
            "Ask the Coder to draft a new shell/python tool.",
            json{
                {"type", "object"},
                {"properties", {
                    {"description", {{"type","string"}}},
                    {"args_schema", {{"type","object"}}},
                }},
                {"required", {"description"}},
            }));
        return out;
    }();
    return v;
}

// ─── schema validation ─────────────────────────────────────────────────────

namespace {

bool typeMatches(const std::string& expected, const json& v) {
    if (expected == "string")  return v.is_string();
    if (expected == "integer") return v.is_number_integer();
    if (expected == "number")  return v.is_number();
    if (expected == "boolean") return v.is_boolean();
    if (expected == "array")   return v.is_array();
    if (expected == "object")  return v.is_object();
    if (expected == "null")    return v.is_null();
    return true;        // unknown type → accept (forward-compat)
}

} // namespace

std::string validateArgsAgainstSchema(const json& schema, const json& args) {
    if (!schema.is_object() || schema.empty()) return {};   // no schema → accept
    std::string root_type = schema.value("type", "object");
    if (root_type != "object") {
        return "args schema root must be 'object'";
    }
    if (!args.is_object()) {
        return "args must be a JSON object";
    }
    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& k : schema["required"]) {
            if (!k.is_string()) continue;
            if (!args.contains(k.get<std::string>())) {
                return "missing required field '" + k.get<std::string>() + "'";
            }
        }
    }
    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (auto it = schema["properties"].begin();
             it != schema["properties"].end(); ++it) {
            if (!args.contains(it.key())) continue;
            if (!it.value().is_object()) continue;
            if (!it.value().contains("type")) continue;
            std::string et = it.value()["type"].get<std::string>();
            if (!typeMatches(et, args[it.key()])) {
                return "field '" + it.key() + "' expected type " + et;
            }
        }
    }
    return {};
}

// ─── impl ──────────────────────────────────────────────────────────────────

struct ToolRegistry::Impl {
    Config cfg;
    mutable std::mutex mu;
    std::vector<ToolInfo> learned;
};

ToolRegistry::ToolRegistry(Config cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = std::move(cfg);
    if (p_->cfg.root.empty()) {
        p_->cfg.root = arise::paths::toolsDir();
    }
    paths::ensureDir(p_->cfg.root);
    paths::ensureDir(learnedDir());
    paths::ensureDir(archivedDir());
    paths::ensureDir(sandboxDir());
    load();
}
ToolRegistry::~ToolRegistry() = default;

std::string ToolRegistry::learnedDir()  const { return p_->cfg.root + "/learned"; }
std::string ToolRegistry::archivedDir() const { return p_->cfg.root + "/archived"; }
std::string ToolRegistry::sandboxDir()  const { return p_->cfg.root + "/sandbox"; }
std::string ToolRegistry::manifestPath() const {
    return learnedDir() + "/manifest.json";
}
std::string ToolRegistry::archivedManifestPath() const {
    return archivedDir() + "/manifest.json";
}

bool ToolRegistry::load() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->learned.clear();
    auto path = manifestPath();
    auto blob = slurp(path);
    if (blob.empty()) return true;        // no manifest is ok
    try {
        auto j = json::parse(blob);
        if (j.contains("tools") && j["tools"].is_array()) {
            for (auto& it : j["tools"]) {
                if (it.is_object()) p_->learned.push_back(fromJson(it));
            }
        }
    } catch (const std::exception& e) {
        log::error(std::string("ToolRegistry: parse manifest: ") + e.what());
        return false;
    }
    return true;
}

bool ToolRegistry::save() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    json j;
    j["version"] = 1;
    j["tools"]   = json::array();
    for (const auto& t : p_->learned) j["tools"].push_back(toJson(t));
    return writeFileAtomic(manifestPath(), j.dump(2));
}

std::vector<ToolInfo> ToolRegistry::listAll(bool include_builtins) const {
    std::vector<ToolInfo> out;
    if (include_builtins) {
        for (auto& t : builtinTable()) out.push_back(t);
    }
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& t : p_->learned) out.push_back(t);
    }
    return out;
}

std::vector<ToolInfo> ToolRegistry::listLearned() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->learned;
}

std::optional<ToolInfo> ToolRegistry::get(const std::string& id) const {
    for (auto& t : builtinTable()) if (t.id == id) return t;
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& t : p_->learned)   if (t.id == id) return t;
    return std::nullopt;
}

bool ToolRegistry::addLearned(ToolInfo tool) {
    if (tool.id.empty()) return false;
    if (tool.id.rfind("builtin:", 0) == 0) return false;
    if (tool.created_at.empty()) tool.created_at = nowIso();
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& existing : p_->learned) {
            if (existing.id == tool.id) {
                existing = std::move(tool);
                goto saved;
            }
        }
        p_->learned.push_back(std::move(tool));
    }
saved:
    return save();
}

bool ToolRegistry::approve(const std::string& id, const std::string& by) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& t : p_->learned) {
            if (t.id == id) {
                t.approved    = true;
                t.approved_at = nowIso();
                t.approved_by = by;
                goto saved;
            }
        }
        return false;
    }
saved:
    return save();
}

bool ToolRegistry::archive(const std::string& id) {
    ToolInfo moved;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        auto it = std::find_if(p_->learned.begin(), p_->learned.end(),
                               [&](const ToolInfo& t){ return t.id == id; });
        if (it == p_->learned.end()) return false;
        moved = std::move(*it);
        p_->learned.erase(it);
    }
    save();

    // Append to archived manifest.
    json arch = json::object();
    arch["version"] = 1;
    arch["tools"]   = json::array();
    auto blob = slurp(archivedManifestPath());
    if (!blob.empty()) {
        try {
            auto j = json::parse(blob);
            if (j.contains("tools") && j["tools"].is_array())
                arch["tools"] = j["tools"];
        } catch (...) {}
    }
    arch["tools"].push_back(toJson(moved));
    writeFileAtomic(archivedManifestPath(), arch.dump(2));
    return true;
}

bool ToolRegistry::remove(const std::string& id) {
    ToolInfo erased;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        auto it = std::find_if(p_->learned.begin(), p_->learned.end(),
                               [&](const ToolInfo& t){ return t.id == id; });
        if (it == p_->learned.end()) return false;
        erased = std::move(*it);
        p_->learned.erase(it);
    }
    save();
    if (!erased.script_path.empty()) {
        std::error_code ec;
        fs::remove(erased.script_path, ec);
    }
    return true;
}

bool ToolRegistry::recordUse(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& t : p_->learned) {
            if (t.id == id) {
                ++t.usage_count;
                t.last_used = nowIso();
                goto saved;
            }
        }
        return false;
    }
saved:
    return save();
}

std::string ToolRegistry::validateArgs(const std::string& id,
                                       const json& args) const {
    auto t = get(id);
    if (!t) return "unknown tool '" + id + "'";
    return validateArgsAgainstSchema(t->args_schema, args);
}

namespace {

// Parse "YYYY-MM-DDTHH:MM:SSZ" → epoch seconds (UTC). Returns 0 on failure.
long long parseIsoUtcEpoch(const std::string& s) {
    if (s.empty()) return 0;
    std::tm tm{};
    if (!strptime(s.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) return 0;
    return static_cast<long long>(timegm(&tm));
}

} // namespace

int ToolRegistry::sweepStale(int older_than_days, bool dry_run) {
    if (older_than_days <= 0) return 0;
    long long now = static_cast<long long>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
    long long cutoff = now - (long long)older_than_days * 86400;

    std::vector<std::string> stale_ids;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (const auto& t : p_->learned) {
            // Use last_used if set, otherwise the older of approved_at /
            // created_at. Never-used unapproved drafts age via created_at.
            std::string ref_iso = t.last_used.empty()
                ? (t.approved_at.empty() ? t.created_at : t.approved_at)
                : t.last_used;
            long long ref = parseIsoUtcEpoch(ref_iso);
            if (ref == 0) continue;     // unparseable → keep
            if (ref < cutoff) stale_ids.push_back(t.id);
        }
    }
    if (dry_run) return int(stale_ids.size());
    int archived = 0;
    for (const auto& id : stale_ids) {
        if (archive(id)) ++archived;
    }
    return archived;
}

std::string ToolRegistry::sanitiseToolId(const std::string& requested) const {
    std::string s;
    s.reserve(requested.size());
    for (char c : requested) {
        if ((c >= 'A' && c <= 'Z')) s.push_back(char(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') ||
                 c == '_')
            s.push_back(c);
        else s.push_back('_');
    }
    // Collapse runs of '_'
    std::string c;
    bool last_us = false;
    for (char ch : s) {
        if (ch == '_') {
            if (!last_us) c.push_back('_');
            last_us = true;
        } else {
            c.push_back(ch);
            last_us = false;
        }
    }
    // Trim leading/trailing '_'
    while (!c.empty() && c.front() == '_') c.erase(c.begin());
    while (!c.empty() && c.back()  == '_') c.pop_back();
    if (c.empty()) c = "tool";

    // Refuse builtin: prefix and dedupe against existing.
    auto exists = [&](const std::string& candidate) {
        if (candidate.rfind("builtin:", 0) == 0) return true;
        for (const auto& bt : builtinTable())
            if (bt.id == "builtin:" + candidate) return true;
        std::lock_guard<std::mutex> lk(p_->mu);
        for (const auto& t : p_->learned)
            if (t.id == candidate) return true;
        return false;
    };

    if (!exists(c)) return c;

    // Append a short random hex suffix until we land on something free.
    static thread_local std::mt19937_64 rng(
        std::random_device{}() ^ std::uint64_t(
            steady_clock::now().time_since_epoch().count()));
    for (int attempt = 0; attempt < 8; ++attempt) {
        char buf[12];
        std::snprintf(buf, sizeof buf, "_%04lx", rng() & 0xFFFF);
        auto candidate = c + buf;
        if (!exists(candidate)) return candidate;
    }
    // Last resort: timestamp suffix
    auto epoch_now = duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();
    return c + "_" + std::to_string(epoch_now);
}

} // namespace arise
