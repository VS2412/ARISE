// `arise` CLI — Phase 1.
//
// Drives the MemoryCortex and IdentityStore directly. No daemon yet; later
// phases turn these into IPC calls to a long-running process. Subcommands
// are deliberately handrolled (no new deps) — the surface is small.

#include "cortex/identity.hpp"
#include "cortex/memory_cortex.hpp"
#include "util/log.hpp"
#include "util/paths.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

using nlohmann::json;
using namespace std::chrono;

namespace {

// ─── small arg helpers ──────────────────────────────────────────────────

struct Args {
    std::vector<std::string> pos;                  // positional
    std::unordered_map<std::string, std::string> opt;  // --key val
    std::unordered_map<std::string, bool>        flag; // --key

    bool has(const std::string& k) const {
        return opt.count(k) > 0 || flag.count(k) > 0;
    }
    std::string get(const std::string& k, std::string def = "") const {
        auto it = opt.find(k);
        return it == opt.end() ? std::move(def) : it->second;
    }
};

Args parseArgs(int argc, char** argv, int start) {
    Args a;
    for (int i = start; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string k = s.substr(2);
            if (i + 1 < argc && std::string(argv[i+1]).rfind("--", 0) != 0) {
                a.opt[k] = argv[++i];
            } else {
                a.flag[k] = true;
            }
        } else {
            a.pos.push_back(std::move(s));
        }
    }
    return a;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// ─── default cortex config ───────────────────────────────────────────────

arise::MemoryCortex::Config defaultCortexConfig() {
    arise::paths::ensureLayout();
    arise::MemoryCortex::Config c;
    c.root              = arise::paths::memoryDir();
    c.embed_cache_path  = arise::paths::cacheDir() + "/embeddings.sqlite";
    c.decay_thread      = false; // CLI runs are short-lived

    auto vec_so = arise::paths::expandHome("~/.local/lib/vec0.so");
    if (arise::paths::fileExists(vec_so)) c.sqlite_vec_path = vec_so;

    if (const char* u = std::getenv("ARIA_OLLAMA_URL")) c.ollama_url = u;
    if (const char* m = std::getenv("ARIA_EMBEDDING_MODEL")) c.embed_model = m;
    if (const char* d = std::getenv("ARIA_EMBEDDING_DIM"))   c.embed_dim = std::atoi(d);
    return c;
}

// ─── help ────────────────────────────────────────────────────────────────

int cmdHelp(int = 0) {
    std::cout <<
"arise — cognitive OS CLI (phase 1)\n"
"\n"
"USAGE\n"
"  arise <command> [args...]\n"
"\n"
"COMMANDS\n"
"  init                              create ~/.arise/ layout + default identity\n"
"\n"
"  mem record --kind K --summary S [--mood M] [--salience F]\n"
"             [--payload PATH] [--decay-days N]\n"
"  mem recall <text> [--limit N] [--types e,s,p]\n"
"  mem dump   [--recent N]\n"
"  mem mood   [show | nudge V A [LABEL] | baseline LABEL | tick]\n"
"  mem purge                          drop decayed events now\n"
"  mem fact   --subject S --predicate P --object O [--confidence F]\n"
"  mem facts  --subject S [--predicate P]\n"
"\n"
"  identity show\n"
"  identity set [--name N] [--pronouns P] [--persona TEXT]\n"
"               [--add-do TEXT] [--add-dont TEXT]\n"
"\n"
"  import-aria [--source PATH] [--dry-run]\n"
"\n"
"ENV\n"
"  ARISE_ROOT          override ~/.arise\n"
"  ARIA_OLLAMA_URL     default http://127.0.0.1:11434\n"
"  ARIA_EMBEDDING_MODEL/DIM\n";
    return 0;
}

// ─── init ────────────────────────────────────────────────────────────────

int cmdInit() {
    auto root = arise::paths::ensureLayout();
    arise::log::init(arise::paths::logsDir() + "/arise.log");
    arise::log::info("arise init: layout at " + root);

    arise::IdentityStore ids(arise::paths::identityDir());
    auto rec = ids.get();
    if (rec.do_list.empty() && rec.dont_list.empty()) {
        rec.do_list  = {"address the user by their preferred name",
                        "be specific and brief",
                        "remember and reuse what worked before"};
        rec.dont_list = {"pad replies with hedging",
                         "claim memory I don't actually have"};
        ids.set(std::move(rec), "initial identity");
    }

    // Touch the cortex once to materialise empty schemas.
    {
        arise::MemoryCortex cortex(defaultCortexConfig());
        (void)cortex.totalSizeBytes();
    }

    std::cout << "ARISE initialised at " << root << "\n";
    return 0;
}

// ─── mem ─────────────────────────────────────────────────────────────────

int cmdMemRecord(const Args& a) {
    arise::EpisodicEvent ev;
    ev.kind    = a.get("kind", "note");
    ev.summary = a.get("summary");
    ev.mood_at = a.get("mood");
    if (a.has("salience")) ev.salience = std::atof(a.get("salience").c_str());
    if (a.has("payload"))  ev.payload_json = slurp(a.get("payload"));
    if (a.has("decay-days")) {
        int d = std::atoi(a.get("decay-days").c_str());
        if (d > 0) ev.decay_at = system_clock::now() + std::chrono::hours(24 * d);
    }
    if (ev.summary.empty()) {
        std::cerr << "mem record: --summary required\n";
        return 2;
    }

    arise::MemoryCortex cortex(defaultCortexConfig());
    auto id = cortex.recordEvent(std::move(ev));
    if (id <= 0) { std::cerr << "mem record: failed\n"; return 1; }
    std::cout << "id=" << id << "\n";
    return 0;
}

std::vector<arise::MemoryType> parseTypes(const std::string& csv) {
    std::vector<arise::MemoryType> out;
    if (csv.empty()) {
        return {arise::MemoryType::Episodic, arise::MemoryType::Semantic};
    }
    std::istringstream iss(csv);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        if      (tok == "e" || tok == "episodic")   out.push_back(arise::MemoryType::Episodic);
        else if (tok == "s" || tok == "semantic")   out.push_back(arise::MemoryType::Semantic);
        else if (tok == "p" || tok == "procedural") out.push_back(arise::MemoryType::Procedural);
    }
    return out;
}

int cmdMemRecall(const Args& a) {
    if (a.pos.empty()) { std::cerr << "mem recall: <text> required\n"; return 2; }
    std::string text = a.pos[0];
    for (size_t i = 1; i < a.pos.size(); ++i) text += " " + a.pos[i];

    arise::RecallQuery q;
    q.text  = text;
    q.limit = a.has("limit") ? std::atoi(a.get("limit").c_str()) : 8;
    q.types = parseTypes(a.get("types"));

    arise::MemoryCortex cortex(defaultCortexConfig());
    auto hits = cortex.recall(q);
    if (hits.empty()) { std::cout << "(no hits)\n"; return 0; }

    for (size_t i = 0; i < hits.size(); ++i) {
        const auto& h = hits[i];
        std::printf("%2zu  [%-10s] score=%.4f  %s\n",
                    i + 1, arise::toString(h.type), h.score, h.rendered.c_str());
    }
    return 0;
}

int cmdMemDump(const Args& a) {
    int n = a.has("recent") ? std::atoi(a.get("recent").c_str()) : 20;
    arise::MemoryCortex cortex(defaultCortexConfig());
    auto evs = cortex.recentEvents(n);
    if (evs.empty()) { std::cout << "(empty)\n"; return 0; }
    for (const auto& ev : evs) {
        std::time_t t = system_clock::to_time_t(ev.ts);
        char ts[24];
        std::tm tm_buf{}; localtime_r(&t, &tm_buf);
        std::strftime(ts, sizeof ts, "%F %T", &tm_buf);
        std::printf("%5lld  %s  %-18s sal=%.2f  %s%s\n",
                    static_cast<long long>(ev.id), ts, ev.kind.c_str(),
                    ev.salience,
                    ev.mood_at.empty() ? "" : ("[" + ev.mood_at + "] ").c_str(),
                    ev.summary.c_str());
    }
    return 0;
}

int cmdMemMood(const Args& a) {
    arise::MemoryCortex cortex(defaultCortexConfig());

    std::string sub = a.pos.empty() ? "show" : a.pos[0];

    if (sub == "show") {
        auto m = cortex.mood();
        std::printf("baseline=%s current=%s valence=%.2f arousal=%.2f\n",
                    m.baseline.c_str(), m.current.c_str(), m.valence, m.arousal);
        return 0;
    }
    if (sub == "tick") {
        cortex.tickMoodDecay();
        return cmdMemMood({{}, {}, {{"_pos","show"}}});  // unused; just show
    }
    if (sub == "baseline" && a.pos.size() >= 2) {
        cortex.setMoodBaseline(a.pos[1]);
        std::cout << "baseline=" << a.pos[1] << "\n";
        return 0;
    }
    if (sub == "nudge" && a.pos.size() >= 3) {
        double v = std::atof(a.pos[1].c_str());
        double ar = std::atof(a.pos[2].c_str());
        std::string label = a.pos.size() >= 4 ? a.pos[3] : "";
        cortex.nudgeMood(v, ar, label);
        auto m = cortex.mood();
        std::printf("nudged → current=%s valence=%.2f arousal=%.2f\n",
                    m.current.c_str(), m.valence, m.arousal);
        return 0;
    }
    std::cerr << "mem mood: unknown subcommand\n";
    return 2;
}

int cmdMemPurge() {
    arise::MemoryCortex cortex(defaultCortexConfig());
    int n = cortex.purgeDecayed();
    std::cout << "purged " << n << " events\n";
    return 0;
}

int cmdMemFact(const Args& a) {
    arise::SemanticFact f;
    f.subject   = a.get("subject");
    f.predicate = a.get("predicate");
    f.object    = a.get("object");
    f.confidence = a.has("confidence")
        ? std::atof(a.get("confidence").c_str()) : 1.0;
    if (f.subject.empty() || f.predicate.empty() || f.object.empty()) {
        std::cerr << "mem fact: --subject, --predicate, --object required\n";
        return 2;
    }
    arise::MemoryCortex cortex(defaultCortexConfig());
    auto id = cortex.upsertFact(std::move(f));
    if (id <= 0) {
        std::cerr << "mem fact: rejected (lower confidence than existing)\n";
        return 1;
    }
    std::cout << "fact id=" << id << "\n";
    return 0;
}

int cmdMemFacts(const Args& a) {
    if (!a.has("subject")) {
        std::cerr << "mem facts: --subject required\n";
        return 2;
    }
    arise::MemoryCortex cortex(defaultCortexConfig());
    auto facts = cortex.queryFacts(a.get("subject"), a.get("predicate"));
    if (facts.empty()) { std::cout << "(none)\n"; return 0; }
    for (const auto& f : facts) {
        std::printf("%4lld  %s %s %s  conf=%.2f\n",
                    static_cast<long long>(f.id),
                    f.subject.c_str(), f.predicate.c_str(),
                    f.object.c_str(), f.confidence);
    }
    return 0;
}

// ─── identity ────────────────────────────────────────────────────────────

int cmdIdentityShow() {
    arise::IdentityStore ids(arise::paths::identityDir());
    auto r = ids.get();
    json j;
    j["name"]            = r.name;
    j["pronouns"]        = r.pronouns;
    j["persona_summary"] = r.persona_summary;
    j["baseline_mood"]   = r.baseline_mood;
    if (r.voice_profile) j["voice_profile"] = *r.voice_profile;
    else                 j["voice_profile"] = nullptr;
    j["do"]   = r.do_list;
    j["dont"] = r.dont_list;
    std::cout << j.dump(2) << "\n";
    return 0;
}

int cmdIdentitySet(const Args& a) {
    arise::IdentityStore ids(arise::paths::identityDir());
    auto r = ids.get();
    if (a.has("name"))     r.name     = a.get("name");
    if (a.has("pronouns")) r.pronouns = a.get("pronouns");
    if (a.has("persona"))  r.persona_summary = a.get("persona");
    if (a.has("add-do"))   r.do_list.push_back(a.get("add-do"));
    if (a.has("add-dont")) r.dont_list.push_back(a.get("add-dont"));
    if (a.has("baseline-mood")) r.baseline_mood = a.get("baseline-mood");
    ids.set(std::move(r), a.get("msg", "update identity via cli"));
    std::cout << "saved.\n";
    return 0;
}

// ─── import-aria ─────────────────────────────────────────────────────────

int cmdImportAria(const Args& a) {
    auto src = a.has("source") ? a.get("source")
                               : arise::paths::expandHome("~/.aria_memory.db");
    if (!arise::paths::fileExists(src)) {
        std::cerr << "import-aria: source not found: " << src << "\n";
        return 1;
    }
    bool dryRun = a.has("dry-run");

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(src.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::cerr << "import-aria: open " << src << " (readonly): "
                  << sqlite3_errmsg(db) << "\n";
        if (db) sqlite3_close(db);
        return 1;
    }

    arise::MemoryCortex cortex(defaultCortexConfig());

    int conv_count = 0, fact_count = 0;
    long long max_seen_aria = 0;

    // ── conversations → episodic ──
    {
        sqlite3_stmt* st = nullptr;
        const char* q = "SELECT id, role, content, timestamp FROM conversations "
                        "ORDER BY id ASC";
        if (sqlite3_prepare_v2(db, q, -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                long long aria_id = sqlite3_column_int64(st, 0);
                std::string role = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                std::string content = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                std::string ts = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                if (content.empty()) continue;

                if (!dryRun) {
                    arise::EpisodicEvent ev;
                    ev.kind    = "conversation_turn";
                    ev.summary = content;
                    json p;
                    p["role"]            = role;
                    p["aria_origin"]     = {{"table", "conversations"}, {"rowid", aria_id}};
                    p["timestamp_local"] = ts;
                    ev.payload_json = p.dump();
                    // Parse ARIA's "%Y-%m-%d %H:%M:%S" if possible.
                    std::tm tm{};
                    if (strptime(ts.c_str(), "%Y-%m-%d %H:%M:%S", &tm)) {
                        tm.tm_isdst = -1;
                        ev.ts = system_clock::from_time_t(std::mktime(&tm));
                    }
                    cortex.recordEvent(std::move(ev));
                }
                ++conv_count;
                if (aria_id > max_seen_aria) max_seen_aria = aria_id;
            }
            sqlite3_finalize(st);
        }
    }

    // ── user_facts → semantic ──
    {
        sqlite3_stmt* st = nullptr;
        const char* q = "SELECT key, value FROM user_facts";
        if (sqlite3_prepare_v2(db, q, -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string key = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                std::string val = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                if (key.empty() || val.empty()) continue;

                if (!dryRun) {
                    arise::SemanticFact f;
                    f.subject   = "user";
                    f.predicate = key;
                    f.object    = val;
                    f.confidence = 1.0;
                    cortex.upsertFact(std::move(f));
                }
                ++fact_count;
            }
            sqlite3_finalize(st);
        }
    }

    sqlite3_close(db);

    std::cout << (dryRun ? "[dry-run] " : "")
              << "conversations imported: " << conv_count
              << ", facts: "                 << fact_count
              << " (max aria_id=" << max_seen_aria << ")\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return cmdHelp();
    std::string cmd = argv[1];
    Args a = parseArgs(argc, argv, 2);

    arise::log::init(arise::paths::logsDir() + "/arise.log");
    arise::log::setLevel(arise::log::Level::Info);
    arise::paths::ensureLayout();

    if (cmd == "help" || cmd == "-h" || cmd == "--help") return cmdHelp();
    if (cmd == "init") return cmdInit();

    if (cmd == "mem") {
        if (a.pos.empty()) { std::cerr << "mem: subcommand required\n"; return 2; }
        std::string sub = a.pos[0];
        Args sa = a;
        sa.pos.erase(sa.pos.begin());
        if (sub == "record") return cmdMemRecord(sa);
        if (sub == "recall") return cmdMemRecall(sa);
        if (sub == "dump")   return cmdMemDump(sa);
        if (sub == "mood")   return cmdMemMood(sa);
        if (sub == "purge")  return cmdMemPurge();
        if (sub == "fact")   return cmdMemFact(sa);
        if (sub == "facts")  return cmdMemFacts(sa);
        std::cerr << "mem: unknown subcommand '" << sub << "'\n";
        return 2;
    }

    if (cmd == "identity") {
        if (a.pos.empty()) { std::cerr << "identity: subcommand required\n"; return 2; }
        std::string sub = a.pos[0];
        Args sa = a; sa.pos.erase(sa.pos.begin());
        if (sub == "show") return cmdIdentityShow();
        if (sub == "set")  return cmdIdentitySet(sa);
        std::cerr << "identity: unknown subcommand '" << sub << "'\n";
        return 2;
    }

    if (cmd == "import-aria") return cmdImportAria(a);

    std::cerr << "unknown command: " << cmd << "\n";
    return cmdHelp();
}
