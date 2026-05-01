#include "cortex/memory_cortex.hpp"
#include "util/embedding_client.hpp"
#include "util/log.hpp"
#include "util/paths.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

namespace arise {

// ─── small helpers ─────────────────────────────────────────────────────────

namespace {

void l2norm(std::vector<float>& v) {
    double s = 0.0;
    for (float x : v) s += double(x) * double(x);
    double n = std::sqrt(s);
    if (n <= 1e-12) return;
    float inv = static_cast<float>(1.0 / n);
    for (float& x : v) x *= inv;
}

double l2ToCos(double d) {
    double cs = 1.0 - (d * d) / 2.0;
    if (cs >  1.0) cs =  1.0;
    if (cs < -1.0) cs = -1.0;
    return cs;
}

long long nowEpoch() {
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string isoOf(system_clock::time_point t) {
    std::time_t tt = system_clock::to_time_t(t);
    std::tm tm_buf{};
    localtime_r(&tt, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof buf, "%FT%T", &tm_buf);
    return buf;
}

std::string friendlyDate(long long epoch) {
    std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof buf, "%a %b %e", &tm_buf);
    std::string s(buf);
    auto dd = s.find("  ");
    if (dd != std::string::npos) s.replace(dd, 2, " ");
    return s;
}

std::string moodLabel(double valence, double arousal) {
    // Two-axis discretiser. Phase 1 only — Phase 4 will swap in something
    // smarter; for now this is enough to give recall and TTS something
    // useful to colour with.
    if (valence < -0.4 && arousal >  0.4) return "frustrated";
    if (valence < -0.4)                   return "down";
    if (valence >  0.4 && arousal >  0.4) return "excited";
    if (valence >  0.4)                   return "warm";
    if (arousal >  0.4)                   return "alert";
    if (arousal < -0.4)                   return "tired";
    return "neutral";
}

// FTS5 query sanitisation: keep alnum+underscore tokens, OR them, quote each
// to bypass the column-prefix syntax. Same shape as ARIA's Memory::search.
std::string buildFtsQuery(const std::string& text) {
    std::ostringstream out;
    std::istringstream iss(text);
    std::string tok;
    bool first = true;
    while (iss >> tok) {
        std::string s;
        for (char c : tok)
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                s += c;
        if (s.empty()) continue;
        if (!first) out << " OR ";
        out << "\"" << s << "\"";
        first = false;
    }
    return out.str();
}

void execOrLog(sqlite3* db, const char* sql, const char* what) {
    if (!db) return;
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        log::error(std::string("MemoryCortex SQL ") + what + ": " +
                   (err ? err : "?"));
        if (err) sqlite3_free(err);
    }
}

} // namespace

const char* toString(MemoryType t) {
    switch (t) {
        case MemoryType::Episodic:   return "episodic";
        case MemoryType::Semantic:   return "semantic";
        case MemoryType::Procedural: return "procedural";
        case MemoryType::Preference: return "preference";
    }
    return "?";
}

// ─── Impl ───────────────────────────────────────────────────────────────────

struct MemoryCortex::Impl {
    Config cfg;

    sqlite3* episodic    = nullptr;
    sqlite3* semantic    = nullptr;
    sqlite3* procedural  = nullptr;
    sqlite3* preferences = nullptr;
    bool     vecEnabled  = false;

    std::unique_ptr<EmbeddingClient> embedder;

    mutable std::mutex epMu, smMu, prMu, pfMu;

    Mood               mood;
    mutable std::mutex moodMu;

    std::atomic<bool>       stopping{false};
    std::condition_variable decayCv;
    std::mutex              decayCvMu;

    explicit Impl(Config c) : cfg(std::move(c)) {}
    ~Impl();

    void open();
    void openEpisodic();
    void openSemantic();
    void openProcedural();
    void openPreferences();
    void loadVecExtension();

    std::string moodFile() const { return cfg.root + "/mood.json"; }
    void        loadMood();
    void        saveMoodLocked();   // expects moodMu held by caller
    void        saveMood();         // takes moodMu
};

MemoryCortex::Impl::~Impl() {
    if (episodic)    sqlite3_close(episodic);
    if (semantic)    sqlite3_close(semantic);
    if (procedural)  sqlite3_close(procedural);
    if (preferences) sqlite3_close(preferences);
}

void MemoryCortex::Impl::open() {
    paths::ensureDir(cfg.root);

    EmbeddingClient::Config ec;
    ec.ollama_url = cfg.ollama_url;
    ec.model      = cfg.embed_model;
    ec.dim        = cfg.embed_dim;
    ec.cache_path = cfg.embed_cache_path;
    embedder = std::make_unique<EmbeddingClient>(ec);

    openEpisodic();
    openSemantic();
    openProcedural();
    openPreferences();

    loadMood();
}

void MemoryCortex::Impl::openEpisodic() {
    auto path = cfg.root + "/episodic.db";
    if (sqlite3_open(path.c_str(), &episodic) != SQLITE_OK) {
        log::error("MemoryCortex: open episodic " + path);
        episodic = nullptr;
        return;
    }

    execOrLog(episodic, R"(
        CREATE TABLE IF NOT EXISTS events (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            ts_epoch      INTEGER NOT NULL,
            ts_iso        TEXT    NOT NULL,
            kind          TEXT    NOT NULL,
            summary       TEXT    NOT NULL,
            payload_json  TEXT,
            salience      REAL    NOT NULL DEFAULT 0.0,
            mood_at       TEXT,
            refs_json     TEXT,
            decay_at      INTEGER
        );
        CREATE INDEX IF NOT EXISTS idx_events_kind     ON events(kind);
        CREATE INDEX IF NOT EXISTS idx_events_decay_at ON events(decay_at);
        CREATE INDEX IF NOT EXISTS idx_events_ts       ON events(ts_epoch);
    )", "events");

    execOrLog(episodic, R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS events_fts USING fts5(
            summary, kind, content='events', content_rowid='id'
        );
        CREATE TRIGGER IF NOT EXISTS events_ai AFTER INSERT ON events BEGIN
            INSERT INTO events_fts(rowid, summary, kind)
                VALUES (new.id, new.summary, new.kind);
        END;
        CREATE TRIGGER IF NOT EXISTS events_ad AFTER DELETE ON events BEGIN
            INSERT INTO events_fts(events_fts, rowid, summary, kind)
                VALUES('delete', old.id, old.summary, old.kind);
        END;
        CREATE TRIGGER IF NOT EXISTS events_au AFTER UPDATE ON events BEGIN
            INSERT INTO events_fts(events_fts, rowid, summary, kind)
                VALUES('delete', old.id, old.summary, old.kind);
            INSERT INTO events_fts(rowid, summary, kind)
                VALUES (new.id, new.summary, new.kind);
        END;
    )", "events_fts");

    loadVecExtension();
    if (vecEnabled) {
        std::string s =
            "CREATE VIRTUAL TABLE IF NOT EXISTS events_vec USING vec0("
            "embedding float[" + std::to_string(cfg.embed_dim) + "]);";
        execOrLog(episodic, s.c_str(), "events_vec");
    }
}

void MemoryCortex::Impl::loadVecExtension() {
    if (!episodic || cfg.sqlite_vec_path.empty()) return;
    if (sqlite3_enable_load_extension(episodic, 1) != SQLITE_OK) {
        log::warn("MemoryCortex: enable_load_extension failed");
        return;
    }
    char* err = nullptr;
    int rc = sqlite3_load_extension(episodic, cfg.sqlite_vec_path.c_str(),
                                    nullptr, &err);
    sqlite3_enable_load_extension(episodic, 0);
    if (rc != SQLITE_OK) {
        log::warn("MemoryCortex: load vec0 (" + cfg.sqlite_vec_path + "): " +
                  (err ? err : "?") + " — FTS-only");
        if (err) sqlite3_free(err);
        return;
    }
    vecEnabled = true;
}

void MemoryCortex::Impl::openSemantic() {
    auto path = cfg.root + "/semantic.db";
    if (sqlite3_open(path.c_str(), &semantic) != SQLITE_OK) {
        log::error("MemoryCortex: open semantic " + path);
        semantic = nullptr;
        return;
    }
    execOrLog(semantic, R"(
        CREATE TABLE IF NOT EXISTS facts (
            id                   INTEGER PRIMARY KEY AUTOINCREMENT,
            subject              TEXT NOT NULL,
            predicate            TEXT NOT NULL,
            object               TEXT NOT NULL,
            confidence           REAL NOT NULL DEFAULT 1.0,
            source_episodic_id   INTEGER,
            last_confirmed_epoch INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_facts_sp ON facts(subject, predicate);
        CREATE INDEX IF NOT EXISTS idx_facts_subject ON facts(subject);

        CREATE VIRTUAL TABLE IF NOT EXISTS facts_fts USING fts5(
            subject, predicate, object,
            content='facts', content_rowid='id'
        );
        CREATE TRIGGER IF NOT EXISTS facts_ai AFTER INSERT ON facts BEGIN
            INSERT INTO facts_fts(rowid, subject, predicate, object)
                VALUES (new.id, new.subject, new.predicate, new.object);
        END;
        CREATE TRIGGER IF NOT EXISTS facts_ad AFTER DELETE ON facts BEGIN
            INSERT INTO facts_fts(facts_fts, rowid, subject, predicate, object)
                VALUES('delete', old.id, old.subject, old.predicate, old.object);
        END;
        CREATE TRIGGER IF NOT EXISTS facts_au AFTER UPDATE ON facts BEGIN
            INSERT INTO facts_fts(facts_fts, rowid, subject, predicate, object)
                VALUES('delete', old.id, old.subject, old.predicate, old.object);
            INSERT INTO facts_fts(rowid, subject, predicate, object)
                VALUES (new.id, new.subject, new.predicate, new.object);
        END;
    )", "facts");
}

void MemoryCortex::Impl::openProcedural() {
    auto path = cfg.root + "/procedural.db";
    if (sqlite3_open(path.c_str(), &procedural) != SQLITE_OK) {
        log::error("MemoryCortex: open procedural " + path);
        procedural = nullptr;
        return;
    }
    execOrLog(procedural, R"(
        CREATE TABLE IF NOT EXISTS procedures (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            goal_pattern    TEXT NOT NULL,
            steps_json      TEXT NOT NULL,
            success_count   INTEGER NOT NULL DEFAULT 0,
            failure_count   INTEGER NOT NULL DEFAULT 0,
            last_used_epoch INTEGER NOT NULL,
            created_epoch   INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_procedures_pattern ON procedures(goal_pattern);
    )", "procedures");
}

void MemoryCortex::Impl::openPreferences() {
    auto path = cfg.root + "/preferences.db";
    if (sqlite3_open(path.c_str(), &preferences) != SQLITE_OK) {
        log::error("MemoryCortex: open preferences " + path);
        preferences = nullptr;
        return;
    }
    execOrLog(preferences, R"(
        CREATE TABLE IF NOT EXISTS preferences (
            key                TEXT PRIMARY KEY,
            value              TEXT NOT NULL,
            weight             REAL NOT NULL DEFAULT 1.0,
            last_updated_epoch INTEGER NOT NULL
        );
    )", "preferences");
}

void MemoryCortex::Impl::loadMood() {
    std::lock_guard<std::mutex> lk(moodMu);
    mood = Mood{};
    mood.last_change_at = system_clock::now();

    std::ifstream f(moodFile());
    if (!f) return;

    try {
        json j;
        f >> j;
        if (j.contains("baseline") && j["baseline"].is_string())
            mood.baseline = j["baseline"].get<std::string>();
        if (j.contains("current") && j["current"].is_string())
            mood.current = j["current"].get<std::string>();
        if (j.contains("valence") && j["valence"].is_number())
            mood.valence = j["valence"].get<double>();
        if (j.contains("arousal") && j["arousal"].is_number())
            mood.arousal = j["arousal"].get<double>();
        if (j.contains("last_change_at_epoch") &&
            j["last_change_at_epoch"].is_number()) {
            long long e = j["last_change_at_epoch"].get<long long>();
            mood.last_change_at = system_clock::from_time_t(static_cast<time_t>(e));
        }
    } catch (const std::exception& e) {
        log::warn("MemoryCortex: mood.json parse: " + std::string(e.what()));
    }
}

void MemoryCortex::Impl::saveMoodLocked() {
    json j;
    j["baseline"] = mood.baseline;
    j["current"]  = mood.current;
    j["valence"]  = mood.valence;
    j["arousal"]  = mood.arousal;
    j["last_change_at_epoch"] = duration_cast<seconds>(
        mood.last_change_at.time_since_epoch()).count();

    auto tmp = moodFile() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            log::warn("MemoryCortex: mood.json open " + tmp);
            return;
        }
        f << j.dump(2);
    }
    std::error_code ec;
    fs::rename(tmp, moodFile(), ec);
    if (ec) log::warn("MemoryCortex: mood.json rename: " + ec.message());
}

void MemoryCortex::Impl::saveMood() {
    std::lock_guard<std::mutex> lk(moodMu);
    saveMoodLocked();
}

// ─── ctor / dtor ────────────────────────────────────────────────────────────

MemoryCortex::MemoryCortex(Config cfg)
    : p_(std::make_unique<Impl>(std::move(cfg))) {
    p_->open();
    if (p_->cfg.decay_thread) {
        decayThread_ = std::thread(&MemoryCortex::decayLoopFn_, this);
    }
}

MemoryCortex::~MemoryCortex() {
    p_->stopping.store(true, std::memory_order_relaxed);
    p_->decayCv.notify_all();
    if (decayThread_.joinable()) decayThread_.join();
}

void MemoryCortex::decayLoopFn_() {
    while (!p_->stopping.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(p_->decayCvMu);
        p_->decayCv.wait_for(
            lk,
            std::chrono::seconds(p_->cfg.decay_check_interval_sec),
            [this] { return p_->stopping.load(std::memory_order_relaxed); });
        if (p_->stopping.load(std::memory_order_relaxed)) break;
        lk.unlock();

        try {
            purgeDecayed();
            tickMoodDecay();
        } catch (const std::exception& e) {
            log::warn(std::string("MemoryCortex: decay loop: ") + e.what());
        }
    }
}

bool MemoryCortex::vecEnabled() const { return p_->vecEnabled; }

// ─── Episodic ───────────────────────────────────────────────────────────────

int64_t MemoryCortex::recordEvent(EpisodicEvent ev) {
    if (!p_->episodic) return 0;
    if (ev.summary.empty() && ev.kind.empty()) return 0;
    if (ev.ts.time_since_epoch().count() == 0) ev.ts = system_clock::now();

    long long ts_epoch = duration_cast<seconds>(ev.ts.time_since_epoch()).count();
    std::string ts_iso = isoOf(ev.ts);

    if (ev.salience <= 0.0) {
        // Heuristic floor until a Curator sub-agent scores events properly:
        // longer summaries, mood tags, and conversation turns get more
        // weight. This keeps "salience 0" events from getting auto-decayed
        // out of existence simply because the writer didn't fill it in.
        double h = 0.3;
        if (ev.summary.size() > 60) h += 0.1;
        if (!ev.mood_at.empty() && ev.mood_at != "neutral") h += 0.2;
        if (ev.kind == "conversation_turn") h += 0.1;
        if (ev.kind == "summary")           h = std::max(h, 0.7);
        ev.salience = std::min(h, 1.0);
    }

    std::optional<long long> decay_epoch;
    if (ev.decay_at) {
        decay_epoch = duration_cast<seconds>(ev.decay_at->time_since_epoch()).count();
    } else {
        if      (ev.salience < 0.3) decay_epoch = ts_epoch + 7  * 86400;
        else if (ev.salience < 0.7) decay_epoch = ts_epoch + 90 * 86400;
        // ≥ 0.7 → permanent.
    }

    json refs = json::array();
    for (auto r : ev.refs) refs.push_back(r);
    std::string refs_str = refs.dump();

    int64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(p_->epMu);
        sqlite3_stmt* st = nullptr;
        const char* q =
            "INSERT INTO events (ts_epoch, ts_iso, kind, summary, payload_json, "
            "salience, mood_at, refs_json, decay_at) VALUES (?,?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(p_->episodic, q, -1, &st, nullptr) != SQLITE_OK) {
            log::error("MemoryCortex: recordEvent prepare");
            return 0;
        }
        sqlite3_bind_int64 (st, 1, ts_epoch);
        sqlite3_bind_text  (st, 2, ts_iso.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (st, 3, ev.kind.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (st, 4, ev.summary.c_str(),   -1, SQLITE_TRANSIENT);
        if (ev.payload_json.empty()) sqlite3_bind_null(st, 5);
        else sqlite3_bind_text(st, 5, ev.payload_json.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 6, ev.salience);
        sqlite3_bind_text  (st, 7, ev.mood_at.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (st, 8, refs_str.c_str(),     -1, SQLITE_TRANSIENT);
        if (decay_epoch) sqlite3_bind_int64(st, 9, *decay_epoch);
        else             sqlite3_bind_null (st, 9);

        if (sqlite3_step(st) == SQLITE_DONE)
            id = sqlite3_last_insert_rowid(p_->episodic);
        sqlite3_finalize(st);
    }

    if (p_->vecEnabled && id > 0 && p_->embedder) {
        std::vector<float> v = ev.embedding.empty()
            ? p_->embedder->embed(ev.summary)
            : ev.embedding;
        if (!v.empty() && static_cast<int>(v.size()) == p_->cfg.embed_dim) {
            l2norm(v);
            std::lock_guard<std::mutex> lk(p_->epMu);
            sqlite3_stmt* st = nullptr;
            const char* q =
                "INSERT OR REPLACE INTO events_vec(rowid, embedding) VALUES (?, ?)";
            if (sqlite3_prepare_v2(p_->episodic, q, -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, id);
                sqlite3_bind_blob (st, 2, v.data(),
                                   static_cast<int>(v.size() * sizeof(float)),
                                   SQLITE_TRANSIENT);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
        }
    }

    return id;
}

namespace {

EpisodicEvent rowToEvent(sqlite3_stmt* st) {
    EpisodicEvent ev;
    ev.id           = sqlite3_column_int64(st, 0);
    ev.ts           = system_clock::from_time_t(
                          static_cast<time_t>(sqlite3_column_int64(st, 1)));
    ev.kind         = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    ev.summary      = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    ev.payload_json = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    ev.salience     = sqlite3_column_double(st, 5);
    ev.mood_at      = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    try {
        auto j = json::parse(reinterpret_cast<const char*>(
                                sqlite3_column_text(st, 7)));
        for (auto& v : j) ev.refs.push_back(v.get<int64_t>());
    } catch (...) {}
    if (sqlite3_column_type(st, 8) != SQLITE_NULL) {
        ev.decay_at = system_clock::from_time_t(
            static_cast<time_t>(sqlite3_column_int64(st, 8)));
    }
    return ev;
}

constexpr const char* kEventCols =
    "id, ts_epoch, kind, summary, COALESCE(payload_json,''), "
    "salience, COALESCE(mood_at,''), COALESCE(refs_json,'[]'), decay_at";

} // namespace

std::optional<EpisodicEvent> MemoryCortex::getEvent(int64_t id) const {
    if (!p_->episodic || id <= 0) return std::nullopt;
    std::lock_guard<std::mutex> lk(p_->epMu);
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string("SELECT ") + kEventCols + " FROM events WHERE id = ?";
    if (sqlite3_prepare_v2(p_->episodic, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_int64(st, 1, id);
    std::optional<EpisodicEvent> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = rowToEvent(st);
    sqlite3_finalize(st);
    return out;
}

std::vector<EpisodicEvent> MemoryCortex::recentEvents(int n) const {
    std::vector<EpisodicEvent> out;
    if (!p_->episodic || n <= 0) return out;
    std::lock_guard<std::mutex> lk(p_->epMu);
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string("SELECT ") + kEventCols +
                      " FROM events ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(p_->episodic, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_int(st, 1, n);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(rowToEvent(st));
    sqlite3_finalize(st);
    std::reverse(out.begin(), out.end());
    return out;
}

int MemoryCortex::purgeDecayed() {
    if (!p_->episodic) return 0;
    long long now = nowEpoch();
    std::lock_guard<std::mutex> lk(p_->epMu);

    int deleted = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(
            p_->episodic,
            "DELETE FROM events WHERE decay_at IS NOT NULL AND decay_at <= ?",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now);
        if (sqlite3_step(st) == SQLITE_DONE)
            deleted = sqlite3_changes(p_->episodic);
        sqlite3_finalize(st);
    }

    // Best-effort vec sweep: drop orphaned rows. INNER JOIN-style cleanup so
    // we don't need triggers across the FTS/vec/main tables.
    if (p_->vecEnabled && deleted > 0) {
        execOrLog(p_->episodic,
            "DELETE FROM events_vec WHERE rowid NOT IN (SELECT id FROM events)",
            "events_vec sweep");
    }

    if (deleted > 0)
        log::info("MemoryCortex: purged " + std::to_string(deleted) + " decayed events");
    return deleted;
}

// ─── Semantic ──────────────────────────────────────────────────────────────

int64_t MemoryCortex::upsertFact(SemanticFact f) {
    if (!p_->semantic) return 0;
    if (f.subject.empty() || f.predicate.empty() || f.object.empty()) return 0;
    f.confidence = std::clamp(f.confidence, 0.0, 1.0);
    long long now = nowEpoch();

    std::lock_guard<std::mutex> lk(p_->smMu);

    int64_t     existing_id   = 0;
    std::string existing_obj;
    double      existing_conf = 0.0;
    {
        sqlite3_stmt* st = nullptr;
        const char* q = "SELECT id, object, confidence FROM facts "
                        "WHERE subject = ? AND predicate = ? "
                        "ORDER BY confidence DESC LIMIT 1";
        if (sqlite3_prepare_v2(p_->semantic, q, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, f.subject.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, f.predicate.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                existing_id   = sqlite3_column_int64(st, 0);
                existing_obj  = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                existing_conf = sqlite3_column_double(st, 2);
            }
            sqlite3_finalize(st);
        }
    }

    if (existing_id == 0) {
        sqlite3_stmt* st = nullptr;
        const char* q = "INSERT INTO facts(subject, predicate, object, confidence, "
                        "source_episodic_id, last_confirmed_epoch) "
                        "VALUES (?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(p_->semantic, q, -1, &st, nullptr) != SQLITE_OK)
            return 0;
        sqlite3_bind_text  (st, 1, f.subject.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (st, 2, f.predicate.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (st, 3, f.object.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 4, f.confidence);
        if (f.source_episodic_id) sqlite3_bind_int64(st, 5, *f.source_episodic_id);
        else                      sqlite3_bind_null (st, 5);
        sqlite3_bind_int64 (st, 6, now);
        sqlite3_step(st);
        int64_t id = sqlite3_last_insert_rowid(p_->semantic);
        sqlite3_finalize(st);
        return id;
    }

    if (existing_obj == f.object) {
        // Reinforce: nudge confidence toward 1.
        double bumped = std::min(1.0, existing_conf + 0.05 * (1.0 - existing_conf));
        sqlite3_stmt* st = nullptr;
        const char* q = "UPDATE facts SET confidence = ?, last_confirmed_epoch = ? "
                        "WHERE id = ?";
        if (sqlite3_prepare_v2(p_->semantic, q, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_double(st, 1, bumped);
            sqlite3_bind_int64 (st, 2, now);
            sqlite3_bind_int64 (st, 3, existing_id);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        return existing_id;
    }

    if (f.confidence >= existing_conf) {
        sqlite3_stmt* st = nullptr;
        const char* q = "UPDATE facts SET object = ?, confidence = ?, "
                        "source_episodic_id = ?, last_confirmed_epoch = ? "
                        "WHERE id = ?";
        if (sqlite3_prepare_v2(p_->semantic, q, -1, &st, nullptr) != SQLITE_OK)
            return existing_id;
        sqlite3_bind_text  (st, 1, f.object.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 2, f.confidence);
        if (f.source_episodic_id) sqlite3_bind_int64(st, 3, *f.source_episodic_id);
        else                      sqlite3_bind_null (st, 3);
        sqlite3_bind_int64 (st, 4, now);
        sqlite3_bind_int64 (st, 5, existing_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        log::info("MemoryCortex: contradicted-overwrote (" + f.subject + "," +
                  f.predicate + "): \"" + existing_obj + "\" -> \"" + f.object + "\"");
        return existing_id;
    }

    log::info("MemoryCortex: rejected lower-confidence contradiction (" +
              f.subject + "," + f.predicate + "): \"" + f.object +
              "\" (conf " + std::to_string(f.confidence) + " < " +
              std::to_string(existing_conf) + ")");
    return 0;
}

std::vector<SemanticFact> MemoryCortex::queryFacts(const std::string& subject,
                                                   const std::string& predicate) const {
    std::vector<SemanticFact> out;
    if (!p_->semantic || subject.empty()) return out;
    std::lock_guard<std::mutex> lk(p_->smMu);
    sqlite3_stmt* st = nullptr;
    const char* q = predicate.empty()
        ? "SELECT id, subject, predicate, object, confidence, source_episodic_id, "
          "last_confirmed_epoch FROM facts WHERE subject = ? "
          "ORDER BY confidence DESC, last_confirmed_epoch DESC"
        : "SELECT id, subject, predicate, object, confidence, source_episodic_id, "
          "last_confirmed_epoch FROM facts WHERE subject = ? AND predicate = ? "
          "ORDER BY confidence DESC, last_confirmed_epoch DESC";
    if (sqlite3_prepare_v2(p_->semantic, q, -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_text(st, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
    if (!predicate.empty())
        sqlite3_bind_text(st, 2, predicate.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        SemanticFact f;
        f.id         = sqlite3_column_int64 (st, 0);
        f.subject    = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        f.predicate  = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        f.object     = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
        f.confidence = sqlite3_column_double(st, 4);
        if (sqlite3_column_type(st, 5) != SQLITE_NULL)
            f.source_episodic_id = sqlite3_column_int64(st, 5);
        f.last_confirmed = system_clock::from_time_t(
            static_cast<time_t>(sqlite3_column_int64(st, 6)));
        out.push_back(std::move(f));
    }
    sqlite3_finalize(st);
    return out;
}

// ─── Procedural ────────────────────────────────────────────────────────────

int64_t MemoryCortex::recordProcedure(Procedure p) {
    if (!p_->procedural || p.goal_pattern.empty()) return 0;
    long long now = nowEpoch();
    if (p.last_used.time_since_epoch().count() == 0)
        p.last_used = system_clock::now();

    std::lock_guard<std::mutex> lk(p_->prMu);
    sqlite3_stmt* st = nullptr;
    const char* q =
        "INSERT INTO procedures(goal_pattern, steps_json, success_count, "
        "failure_count, last_used_epoch, created_epoch) VALUES (?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(p_->procedural, q, -1, &st, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_text (st, 1, p.goal_pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, p.steps_json.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 3, p.success_count);
    sqlite3_bind_int  (st, 4, p.failure_count);
    sqlite3_bind_int64(st, 5, duration_cast<seconds>(
                                  p.last_used.time_since_epoch()).count());
    sqlite3_bind_int64(st, 6, now);
    sqlite3_step(st);
    int64_t id = sqlite3_last_insert_rowid(p_->procedural);
    sqlite3_finalize(st);
    return id;
}

std::vector<Procedure> MemoryCortex::matchProcedures(const std::string& goal_pattern,
                                                     int n) const {
    std::vector<Procedure> out;
    if (!p_->procedural || goal_pattern.empty() || n <= 0) return out;
    std::lock_guard<std::mutex> lk(p_->prMu);
    sqlite3_stmt* st = nullptr;
    const char* q =
        "SELECT id, goal_pattern, steps_json, success_count, failure_count, "
        "       last_used_epoch FROM procedures WHERE goal_pattern LIKE ? "
        "ORDER BY (success_count - failure_count) DESC, last_used_epoch DESC LIMIT ?";
    if (sqlite3_prepare_v2(p_->procedural, q, -1, &st, nullptr) != SQLITE_OK)
        return out;
    std::string like = "%" + goal_pattern + "%";
    sqlite3_bind_text(st, 1, like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 2, n);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Procedure p;
        p.id            = sqlite3_column_int64(st, 0);
        p.goal_pattern  = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        p.steps_json    = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        p.success_count = sqlite3_column_int(st, 3);
        p.failure_count = sqlite3_column_int(st, 4);
        p.last_used     = system_clock::from_time_t(
            static_cast<time_t>(sqlite3_column_int64(st, 5)));
        out.push_back(std::move(p));
    }
    sqlite3_finalize(st);
    return out;
}

void MemoryCortex::bumpProcedure(int64_t id, bool succeeded) {
    if (!p_->procedural || id <= 0) return;
    std::lock_guard<std::mutex> lk(p_->prMu);
    sqlite3_stmt* st = nullptr;
    const char* q = succeeded
        ? "UPDATE procedures SET success_count = success_count + 1, "
          "last_used_epoch = ? WHERE id = ?"
        : "UPDATE procedures SET failure_count = failure_count + 1, "
          "last_used_epoch = ? WHERE id = ?";
    if (sqlite3_prepare_v2(p_->procedural, q, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, nowEpoch());
        sqlite3_bind_int64(st, 2, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

// ─── Preferences ───────────────────────────────────────────────────────────

void MemoryCortex::setPreference(Preference p) {
    if (!p_->preferences || p.key.empty()) return;
    long long now = nowEpoch();
    std::lock_guard<std::mutex> lk(p_->pfMu);
    sqlite3_stmt* st = nullptr;
    const char* q = "INSERT INTO preferences(key, value, weight, last_updated_epoch) "
                    "VALUES(?,?,?,?) "
                    "ON CONFLICT(key) DO UPDATE SET "
                    "  value = excluded.value, "
                    "  weight = excluded.weight, "
                    "  last_updated_epoch = excluded.last_updated_epoch";
    if (sqlite3_prepare_v2(p_->preferences, q, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text  (st, 1, p.key.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (st, 2, p.value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 3, p.weight);
    sqlite3_bind_int64 (st, 4, now);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::optional<Preference> MemoryCortex::getPreference(const std::string& key) const {
    if (!p_->preferences || key.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lk(p_->pfMu);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->preferences,
            "SELECT value, weight, last_updated_epoch FROM preferences WHERE key = ?",
            -1, &st, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<Preference> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        Preference p;
        p.key          = key;
        p.value        = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        p.weight       = sqlite3_column_double(st, 1);
        p.last_updated = system_clock::from_time_t(
            static_cast<time_t>(sqlite3_column_int64(st, 2)));
        out = std::move(p);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<Preference> MemoryCortex::listPreferences() const {
    std::vector<Preference> out;
    if (!p_->preferences) return out;
    std::lock_guard<std::mutex> lk(p_->pfMu);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->preferences,
            "SELECT key, value, weight, last_updated_epoch FROM preferences "
            "ORDER BY weight DESC, last_updated_epoch DESC",
            -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        Preference p;
        p.key          = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        p.value        = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        p.weight       = sqlite3_column_double(st, 2);
        p.last_updated = system_clock::from_time_t(
            static_cast<time_t>(sqlite3_column_int64(st, 3)));
        out.push_back(std::move(p));
    }
    sqlite3_finalize(st);
    return out;
}

// ─── Mood ──────────────────────────────────────────────────────────────────

Mood MemoryCortex::mood() const {
    std::lock_guard<std::mutex> lk(p_->moodMu);
    return p_->mood;
}

void MemoryCortex::setMoodBaseline(const std::string& label) {
    {
        std::lock_guard<std::mutex> lk(p_->moodMu);
        p_->mood.baseline = label;
    }
    p_->saveMood();
}

void MemoryCortex::nudgeMood(double dV, double dA, std::string label) {
    {
        std::lock_guard<std::mutex> lk(p_->moodMu);
        p_->mood.valence = std::clamp(p_->mood.valence + dV, -1.0, 1.0);
        p_->mood.arousal = std::clamp(p_->mood.arousal + dA, -1.0, 1.0);
        p_->mood.current = label.empty()
            ? moodLabel(p_->mood.valence, p_->mood.arousal)
            : std::move(label);
        p_->mood.last_change_at = system_clock::now();
        p_->saveMoodLocked();
    }
}

void MemoryCortex::tickMoodDecay() {
    long long ago_s;
    double    valence_now, arousal_now;
    {
        std::lock_guard<std::mutex> lk(p_->moodMu);
        ago_s = duration_cast<seconds>(
            system_clock::now() - p_->mood.last_change_at).count();
        valence_now = p_->mood.valence;
        arousal_now = p_->mood.arousal;
    }
    if (ago_s <= 0) return;
    double half_life = p_->cfg.mood_half_life_seconds;
    if (half_life <= 0) return;

    double decay = std::pow(0.5, double(ago_s) / half_life);
    double new_v = valence_now * decay;
    double new_a = arousal_now * decay;

    {
        std::lock_guard<std::mutex> lk(p_->moodMu);
        p_->mood.valence = new_v;
        p_->mood.arousal = new_a;
        p_->mood.current = moodLabel(new_v, new_a);
        p_->mood.last_change_at = system_clock::now();
        p_->saveMoodLocked();
    }
}

// ─── Recall (FTS5 ∪ Vec → RRF + recency) ───────────────────────────────────

std::vector<RecallHit> MemoryCortex::recall(const RecallQuery& q) const {
    std::vector<RecallHit> hits;
    if (q.text.empty() || q.limit <= 0) return hits;

    bool wantEpi = false, wantSem = false, wantProc = false;
    for (auto t : q.types) {
        if (t == MemoryType::Episodic)   wantEpi  = true;
        if (t == MemoryType::Semantic)   wantSem  = true;
        if (t == MemoryType::Procedural) wantProc = true;
    }

    const int perSource = std::max(q.limit * 3, 10);
    std::string ftsQ = buildFtsQuery(q.text);
    long long now = nowEpoch();

    // ── Episodic ──
    struct EpScore { double score = 0.0; double cos = 0.0; std::string source; };
    std::unordered_map<int64_t, EpScore> ep;

    if (wantEpi && p_->episodic && !ftsQ.empty()) {
        std::lock_guard<std::mutex> lk(p_->epMu);
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT e.id FROM events e JOIN events_fts f ON e.id = f.rowid "
            "WHERE events_fts MATCH ? ORDER BY rank LIMIT ?";
        if (sqlite3_prepare_v2(p_->episodic, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, ftsQ.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (st, 2, perSource);
            int rank = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(st, 0);
                double  rrf = 1.0 / (60.0 + double(++rank));
                auto& e = ep[id];
                e.score += rrf;
                e.source = "fts";
            }
            sqlite3_finalize(st);
        }
    }

    if (wantEpi && p_->vecEnabled && p_->embedder) {
        auto qv = p_->embedder->embed(q.text);
        if (!qv.empty() && static_cast<int>(qv.size()) == p_->cfg.embed_dim) {
            std::lock_guard<std::mutex> lk(p_->epMu);
            sqlite3_stmt* st = nullptr;
            const char* sql =
                "SELECT rowid, distance FROM events_vec "
                "WHERE embedding MATCH ? ORDER BY distance LIMIT ?";
            if (sqlite3_prepare_v2(p_->episodic, sql, -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_blob(st, 1, qv.data(),
                                  static_cast<int>(qv.size() * sizeof(float)),
                                  SQLITE_TRANSIENT);
                sqlite3_bind_int (st, 2, perSource);
                int rank = 0;
                while (sqlite3_step(st) == SQLITE_ROW) {
                    int64_t id   = sqlite3_column_int64 (st, 0);
                    double  dist = sqlite3_column_double(st, 1);
                    double  rrf  = 1.0 / (60.0 + double(++rank));
                    auto& e = ep[id];
                    e.cos    = l2ToCos(dist);
                    e.score += rrf;
                    e.source = e.source.empty() ? "vec" : "both";
                }
                sqlite3_finalize(st);
            }
        }
    }

    for (auto& [id, sc] : ep) {
        auto ev = getEvent(id);
        if (!ev) continue;
        long long ts = duration_cast<seconds>(ev->ts.time_since_epoch()).count();
        double ageDays = ts > 0 ? double(now - ts) / 86400.0 : 365.0;
        double recency = std::exp(-ageDays / 30.0);
        // RRF max ≈ 0.016 per source; pick a recency scale that's a
        // tie-breaker rather than a dominator.
        double final_score = 0.6 * sc.score + 0.4 * recency * 0.03;

        RecallHit h;
        h.type     = MemoryType::Episodic;
        h.score    = final_score;
        h.when     = ev->ts;
        std::string mood = ev->mood_at.empty() ? "" : " [" + ev->mood_at + "]";
        h.rendered = friendlyDate(ts) + mood + ": " + ev->summary;
        h.episodic = std::move(ev);
        hits.push_back(std::move(h));
    }

    // ── Semantic ──
    if (wantSem && p_->semantic && !ftsQ.empty()) {
        std::lock_guard<std::mutex> lk(p_->smMu);
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT f.id, f.subject, f.predicate, f.object, f.confidence, "
            "       f.source_episodic_id, f.last_confirmed_epoch "
            "FROM facts f JOIN facts_fts ff ON f.id = ff.rowid "
            "WHERE facts_fts MATCH ? ORDER BY rank LIMIT ?";
        if (sqlite3_prepare_v2(p_->semantic, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, ftsQ.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (st, 2, perSource);
            int rank = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                SemanticFact f;
                f.id         = sqlite3_column_int64 (st, 0);
                f.subject    = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                f.predicate  = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                f.object     = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                f.confidence = sqlite3_column_double(st, 4);
                if (sqlite3_column_type(st, 5) != SQLITE_NULL)
                    f.source_episodic_id = sqlite3_column_int64(st, 5);
                f.last_confirmed = system_clock::from_time_t(
                    static_cast<time_t>(sqlite3_column_int64(st, 6)));

                double rrf = 1.0 / (60.0 + double(++rank));
                RecallHit h;
                h.type     = MemoryType::Semantic;
                h.score    = 0.6 * rrf + 0.4 * f.confidence * 0.03;
                h.when     = f.last_confirmed;
                h.rendered = "Fact: " + f.subject + " " + f.predicate + " " + f.object;
                h.fact     = std::move(f);
                hits.push_back(std::move(h));
            }
            sqlite3_finalize(st);
        }
    }

    // ── Procedural (substring; richer matching comes when we reuse it) ──
    if (wantProc && p_->procedural) {
        std::lock_guard<std::mutex> lk(p_->prMu);
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT goal_pattern, success_count, failure_count, last_used_epoch "
            "FROM procedures WHERE goal_pattern LIKE ? "
            "ORDER BY (success_count - failure_count) DESC LIMIT ?";
        std::string like = "%" + q.text + "%";
        if (sqlite3_prepare_v2(p_->procedural, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, like.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (st, 2, perSource);
            int rank = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string pat = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                int sc = sqlite3_column_int(st, 1);
                int fc = sqlite3_column_int(st, 2);
                long long lu = sqlite3_column_int64(st, 3);

                double rrf = 1.0 / (60.0 + double(++rank));
                RecallHit h;
                h.type     = MemoryType::Procedural;
                h.score    = 0.6 * rrf;
                h.when     = system_clock::from_time_t(static_cast<time_t>(lu));
                h.rendered = "Procedure: " + pat + " (success " +
                             std::to_string(sc) + "/" +
                             std::to_string(sc + fc) + ")";
                hits.push_back(std::move(h));
            }
            sqlite3_finalize(st);
        }
    }

    std::sort(hits.begin(), hits.end(),
              [](const RecallHit& a, const RecallHit& b) {
                  return a.score > b.score;
              });
    if (static_cast<int>(hits.size()) > q.limit) hits.resize(q.limit);
    return hits;
}

// ─── Maintenance ───────────────────────────────────────────────────────────

void MemoryCortex::compact() {
    {
        std::lock_guard<std::mutex> lk(p_->epMu);
        execOrLog(p_->episodic,    "VACUUM",  "vacuum episodic");
    }
    {
        std::lock_guard<std::mutex> lk(p_->smMu);
        execOrLog(p_->semantic,    "VACUUM",  "vacuum semantic");
    }
    {
        std::lock_guard<std::mutex> lk(p_->prMu);
        execOrLog(p_->procedural,  "VACUUM",  "vacuum procedural");
    }
    {
        std::lock_guard<std::mutex> lk(p_->pfMu);
        execOrLog(p_->preferences, "VACUUM",  "vacuum preferences");
    }
}

int64_t MemoryCortex::totalSizeBytes() const {
    int64_t total = 0;
    for (const std::string& f :
         {p_->cfg.root + "/episodic.db",
          p_->cfg.root + "/semantic.db",
          p_->cfg.root + "/procedural.db",
          p_->cfg.root + "/preferences.db",
          p_->cfg.root + "/mood.json"}) {
        std::error_code ec;
        auto sz = fs::file_size(f, ec);
        if (!ec) total += static_cast<int64_t>(sz);
    }
    return total;
}

} // namespace arise
