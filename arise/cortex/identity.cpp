#include "cortex/identity.hpp"
#include "util/log.hpp"
#include "util/paths.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

const char* kDefaultPersona =
    "Direct but warm. Doesn't pad replies. Remembers.";

std::string nowIso() {
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof buf, "%FT%T", &tm_buf);
    return buf;
}

system_clock::time_point parseIso(const std::string& s) {
    if (s.empty()) return {};
    std::tm tm{};
    if (strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm) == nullptr) {
        if (strptime(s.c_str(), "%Y-%m-%d %H:%M:%S", &tm) == nullptr) return {};
    }
    tm.tm_isdst = -1;
    return system_clock::from_time_t(std::mktime(&tm));
}

// Quote a string for inline use in a `git commit -m "..."` shell command.
std::string shellEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '"' || c == '\\' || c == '`' || c == '$') out += '\\';
        out += c;
    }
    return out;
}

} // namespace

IdentityStore::IdentityStore(std::string d) : dir_(std::move(d)) {
    paths::ensureDir(dir_);
    git_ok_ = (std::system("git --version >/dev/null 2>&1") == 0);
    if (git_ok_) initRepo_();
}

IdentityStore::~IdentityStore() = default;

std::string IdentityStore::filePath() const { return dir_ + "/identity.json"; }

void IdentityStore::initRepo_() {
    if (fs::exists(dir_ + "/.git")) return;
    std::ostringstream cmd;
    cmd << "git -C " << dir_ << " init -q >/dev/null 2>&1";
    std::system(cmd.str().c_str());
    log::info("IdentityStore: initialised git repo at " + dir_);
}

void IdentityStore::commit_(const std::string& msg) {
    if (!git_ok_) return;
    if (!fs::exists(filePath())) return;

    // Per-invocation user.email/user.name + commit.gpgsign=false so a missing
    // global git user / unconfigured GPG never breaks identity persistence.
    const std::string base =
        "git -C " + dir_ +
        " -c user.email=arise@local -c user.name=ARISE -c commit.gpgsign=false ";

    std::ostringstream add;
    add << base << "add identity.json >/dev/null 2>&1";
    std::system(add.str().c_str());

    std::ostringstream commit;
    commit << base << "commit -q -m \"" << shellEscape(msg) << "\" >/dev/null 2>&1";
    std::system(commit.str().c_str());
}

IdentityRecord IdentityStore::get() const {
    IdentityRecord rec;
    rec.persona_summary = kDefaultPersona;
    rec.created_at = system_clock::now();
    rec.updated_at = rec.created_at;

    std::ifstream f(filePath());
    if (!f) return rec;

    try {
        json j;
        f >> j;
        if (j.contains("version") && j["version"].is_number_integer())
            rec.version = j["version"].get<int>();
        if (j.contains("name") && j["name"].is_string())
            rec.name = j["name"].get<std::string>();
        if (j.contains("pronouns") && j["pronouns"].is_string())
            rec.pronouns = j["pronouns"].get<std::string>();
        if (j.contains("persona_summary") && j["persona_summary"].is_string())
            rec.persona_summary = j["persona_summary"].get<std::string>();
        if (j.contains("baseline_mood") && j["baseline_mood"].is_string())
            rec.baseline_mood = j["baseline_mood"].get<std::string>();
        if (j.contains("voice_profile") && j["voice_profile"].is_string())
            rec.voice_profile = j["voice_profile"].get<std::string>();
        if (j.contains("do") && j["do"].is_array())
            for (auto& v : j["do"])
                if (v.is_string()) rec.do_list.push_back(v.get<std::string>());
        if (j.contains("dont") && j["dont"].is_array())
            for (auto& v : j["dont"])
                if (v.is_string()) rec.dont_list.push_back(v.get<std::string>());
        if (j.contains("created_at") && j["created_at"].is_string())
            rec.created_at = parseIso(j["created_at"].get<std::string>());
        if (j.contains("updated_at") && j["updated_at"].is_string())
            rec.updated_at = parseIso(j["updated_at"].get<std::string>());
    } catch (const std::exception& e) {
        log::warn("IdentityStore: parse error: " + std::string(e.what()));
    }
    return rec;
}

void IdentityStore::set(IdentityRecord rec, const std::string& commit_msg) {
    // Preserve original created_at if a file already exists.
    std::string created_iso = nowIso();
    {
        std::ifstream f(filePath());
        if (f) {
            try {
                json prev;
                f >> prev;
                if (prev.contains("created_at") && prev["created_at"].is_string())
                    created_iso = prev["created_at"].get<std::string>();
            } catch (...) { /* keep nowIso() */ }
        }
    }

    rec.updated_at = system_clock::now();

    json j;
    j["version"]         = rec.version;
    j["name"]            = rec.name;
    j["pronouns"]        = rec.pronouns;
    j["persona_summary"] = rec.persona_summary;
    j["baseline_mood"]   = rec.baseline_mood;
    if (rec.voice_profile) j["voice_profile"] = *rec.voice_profile;
    else                   j["voice_profile"] = nullptr;
    j["do"]              = rec.do_list;
    j["dont"]            = rec.dont_list;
    j["created_at"]      = created_iso;
    j["updated_at"]      = nowIso();

    auto path = filePath();
    auto tmp  = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            log::warn("IdentityStore: open " + tmp);
            return;
        }
        f << j.dump(2);
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        log::warn("IdentityStore: rename failed: " + ec.message());
        return;
    }

    commit_(commit_msg);
}

} // namespace arise
