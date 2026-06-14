#include "cortex/adapter_registry.hpp"

#include "util/log.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

namespace arise {

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

json toJson(const AdapterInfo& a) {
    json j;
    j["id"]              = a.id;
    j["base_model"]      = a.base_model;
    j["path"]            = a.path;
    j["eval_score"]      = a.eval_score;
    j["baseline_score"]  = a.baseline_score;
    j["created_at"]      = isoOf(a.created_at);
    j["evaluated_at"]    = isoOf(a.evaluated_at);
    j["deployed"]        = a.deployed;
    j["rolled_back"]     = a.rolled_back;
    j["note"]            = a.note;
    return j;
}

AdapterInfo fromJson(const json& j) {
    AdapterInfo a;
    a.id              = j.value("id", "");
    a.base_model      = j.value("base_model", "");
    a.path            = j.value("path", "");
    a.eval_score      = j.value("eval_score",     0.0);
    a.baseline_score  = j.value("baseline_score", 0.0);
    a.created_at      = parseIso(j.value("created_at",   ""));
    a.evaluated_at    = parseIso(j.value("evaluated_at", ""));
    a.deployed        = j.value("deployed",   false);
    a.rolled_back     = j.value("rolled_back", false);
    a.note            = j.value("note",       "");
    return a;
}

} // namespace

struct AdapterRegistry::Impl {
    Config              cfg;
    mutable std::mutex  mu;
    std::vector<AdapterInfo> adapters;
};

AdapterRegistry::AdapterRegistry(Config cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = std::move(cfg);
    if (p_->cfg.manifest_path.empty()) {
        log::error("AdapterRegistry: empty manifest_path");
        return;
    }
    fs::create_directories(fs::path(p_->cfg.manifest_path).parent_path());
    load();
}

AdapterRegistry::~AdapterRegistry() = default;

std::string AdapterRegistry::path() const { return p_->cfg.manifest_path; }

bool AdapterRegistry::load() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->adapters.clear();
    auto blob = slurp(p_->cfg.manifest_path);
    if (blob.empty()) return true;
    try {
        auto j = json::parse(blob);
        if (j.contains("adapters") && j["adapters"].is_array()) {
            for (auto& a : j["adapters"]) {
                if (a.is_object()) p_->adapters.push_back(fromJson(a));
            }
        }
    } catch (const std::exception& e) {
        log::error(std::string("AdapterRegistry: parse: ") + e.what());
        return false;
    }
    return true;
}

bool AdapterRegistry::save() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    json j;
    j["version"]  = 1;
    j["adapters"] = json::array();
    for (const auto& a : p_->adapters) j["adapters"].push_back(toJson(a));
    return writeFileAtomic(p_->cfg.manifest_path, j.dump(2));
}

bool AdapterRegistry::registerAdapter(AdapterInfo info) {
    if (info.id.empty()) return false;
    if (info.created_at.time_since_epoch().count() == 0) {
        info.created_at = system_clock::now();
    }
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& a : p_->adapters) {
            if (a.id == info.id) {
                // Preserve the existing deployed/rolled_back flags unless
                // the caller is explicitly setting them.
                if (!info.deployed)    info.deployed    = a.deployed;
                if (!info.rolled_back) info.rolled_back = a.rolled_back;
                a = std::move(info);
                goto saved;
            }
        }
        p_->adapters.push_back(std::move(info));
    }
saved:
    return save();
}

bool AdapterRegistry::promote(const std::string& id) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& a : p_->adapters) {
            if (a.id == id) {
                a.deployed    = true;
                a.rolled_back = false;
                found = true;
            } else {
                a.deployed = false;
            }
        }
    }
    if (!found) return false;
    return save();
}

bool AdapterRegistry::rollback(const std::string& id) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& a : p_->adapters) {
            if (a.id == id) {
                a.deployed    = false;
                a.rolled_back = true;
                found = true;
            }
        }
    }
    if (!found) return false;
    return save();
}

bool AdapterRegistry::recordEval(const std::string& id, double eval_score,
                                 double baseline_score, std::string note) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& a : p_->adapters) {
            if (a.id == id) {
                a.eval_score     = eval_score;
                a.baseline_score = baseline_score;
                a.evaluated_at   = system_clock::now();
                if (!note.empty()) a.note = std::move(note);
                found = true;
                break;
            }
        }
    }
    if (!found) return false;
    return save();
}

int AdapterRegistry::rotate() {
    int removed = 0;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        if (p_->cfg.keep <= 0) return 0;
        // Sort by created_at descending; keep the first `keep` plus any
        // currently-deployed row (defensive — should already be in the
        // top of the list, but allow long-deployed legacy rows).
        std::sort(p_->adapters.begin(), p_->adapters.end(),
                  [](const AdapterInfo& a, const AdapterInfo& b) {
                      return a.created_at > b.created_at;
                  });
        std::vector<AdapterInfo> kept;
        kept.reserve(p_->adapters.size());
        int idx = 0;
        for (auto& a : p_->adapters) {
            if (idx < p_->cfg.keep || a.deployed) {
                kept.push_back(std::move(a));
            } else {
                ++removed;
            }
            ++idx;
        }
        p_->adapters = std::move(kept);
    }
    if (removed > 0) save();
    return removed;
}

std::optional<AdapterInfo>
AdapterRegistry::get(const std::string& id) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const auto& a : p_->adapters) if (a.id == id) return a;
    return std::nullopt;
}

std::optional<AdapterInfo> AdapterRegistry::deployed() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const auto& a : p_->adapters) if (a.deployed) return a;
    return std::nullopt;
}

std::vector<AdapterInfo> AdapterRegistry::list() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->adapters;
}

} // namespace arise
