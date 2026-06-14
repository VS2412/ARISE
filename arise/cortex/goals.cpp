#include "cortex/goals.hpp"
#include "cortex/memory_cortex.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <mutex>
#include <queue>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

using nlohmann::json;
using namespace std::chrono;

namespace arise {

// ─── enum bridges ──────────────────────────────────────────────────────────

const char* toString(GoalStatus s) {
    switch (s) {
        case GoalStatus::Proposed:   return "proposed";
        case GoalStatus::Accepted:   return "accepted";
        case GoalStatus::InProgress: return "in_progress";
        case GoalStatus::Blocked:    return "blocked";
        case GoalStatus::Done:       return "done";
        case GoalStatus::Rejected:   return "rejected";
        case GoalStatus::Cancelled:  return "cancelled";
    }
    return "proposed";
}

std::optional<GoalStatus> goalStatusFromString(std::string_view s) {
    if (s == "proposed")    return GoalStatus::Proposed;
    if (s == "accepted")    return GoalStatus::Accepted;
    if (s == "in_progress") return GoalStatus::InProgress;
    if (s == "blocked")     return GoalStatus::Blocked;
    if (s == "done")        return GoalStatus::Done;
    if (s == "rejected")    return GoalStatus::Rejected;
    if (s == "cancelled")   return GoalStatus::Cancelled;
    return std::nullopt;
}

bool isValidTransition(GoalStatus from, GoalStatus to) {
    if (from == to) return true;       // idempotent
    switch (from) {
        case GoalStatus::Proposed:
            return to == GoalStatus::Accepted
                || to == GoalStatus::Rejected
                || to == GoalStatus::Cancelled;
        case GoalStatus::Accepted:
            return to == GoalStatus::InProgress
                || to == GoalStatus::Blocked
                || to == GoalStatus::Done
                || to == GoalStatus::Cancelled;
        case GoalStatus::InProgress:
            return to == GoalStatus::Blocked
                || to == GoalStatus::Done
                || to == GoalStatus::Cancelled;
        case GoalStatus::Blocked:
            return to == GoalStatus::InProgress
                || to == GoalStatus::Done
                || to == GoalStatus::Cancelled;
        case GoalStatus::Done:
        case GoalStatus::Rejected:
        case GoalStatus::Cancelled:
            return false;              // terminal
    }
    return false;
}

// ─── helpers ───────────────────────────────────────────────────────────────

namespace {

long long nowEpoch() {
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

GoalTimestamp tpFromEpoch(long long e) {
    return GoalTimestamp(seconds(e));
}

void execOrLog(sqlite3* db, const char* sql, const char* what) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        log::error(std::string("GoalStore SQL ") + what + ": " + (err ? err : "?"));
        if (err) sqlite3_free(err);
    }
}

// FTS5 query sanitiser — same shape as MemoryCortex's. Whitespace-tokenised,
// alphanum/underscore preserved, OR'd, each token quoted to bypass column
// prefixes. Empty input → empty string (caller treats as "no filter").
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

std::string serialiseTags(const std::vector<std::string>& tags) {
    json j = tags;
    return j.dump();
}

std::vector<std::string> deserialiseTags(const std::string& blob) {
    if (blob.empty()) return {};
    try {
        auto j = json::parse(blob);
        if (j.is_array()) return j.get<std::vector<std::string>>();
    } catch (...) {}
    return {};
}

// Read row from a stmt that selected the canonical column order (see kCols).
// ResultColumns: 0 id, 1 summary, 2 status, 3 priority, 4 deadline_at,
//                5 parent_id, 6 created_at, 7 last_progress_at,
//                8 blocked_reason, 9 plan_json, 10 tags_json
//
// Every column is `g.`-prefixed so this list is safe to drop into queries
// that JOIN goals_fts (which has its own `summary` and `blocked_reason`
// columns and would otherwise make the bare names ambiguous). All callers
// alias the goals table as `g`.
constexpr const char* kSelectCols =
    "g.id, g.summary, g.status, g.priority, g.deadline_at, g.parent_id, "
    "g.created_at, g.last_progress_at, g.blocked_reason, g.plan_json, "
    "g.tags_json";

Goal readRow(sqlite3_stmt* st) {
    Goal g;
    g.id      = sqlite3_column_int64(st, 0);
    g.summary = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    auto status_str = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    if (status_str) {
        if (auto s = goalStatusFromString(status_str)) g.status = *s;
    }
    g.priority = sqlite3_column_int(st, 3);
    if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
        g.deadline_at = tpFromEpoch(sqlite3_column_int64(st, 4));
    }
    if (sqlite3_column_type(st, 5) != SQLITE_NULL) {
        g.parent_id = sqlite3_column_int64(st, 5);
    }
    g.created_at        = tpFromEpoch(sqlite3_column_int64(st, 6));
    g.last_progress_at  = tpFromEpoch(sqlite3_column_int64(st, 7));
    if (auto* br = reinterpret_cast<const char*>(sqlite3_column_text(st, 8))) {
        g.blocked_reason = br;
    }
    if (auto* pj = reinterpret_cast<const char*>(sqlite3_column_text(st, 9))) {
        g.plan_json = pj;
    }
    if (auto* tj = reinterpret_cast<const char*>(sqlite3_column_text(st, 10))) {
        g.tags = deserialiseTags(tj);
    }
    return g;
}

} // namespace

// ─── impl ──────────────────────────────────────────────────────────────────

struct GoalStore::Impl {
    Config              cfg;
    sqlite3*            db = nullptr;
    mutable std::mutex  mu;

    void initSchema() {
        execOrLog(db, "PRAGMA foreign_keys = ON;",  "pragma fk");
        execOrLog(db, "PRAGMA journal_mode = WAL;", "pragma wal");
        execOrLog(db,
            "CREATE TABLE IF NOT EXISTS goals ("
            "  id                INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  summary           TEXT NOT NULL,"
            "  status            TEXT NOT NULL,"
            "  priority          INTEGER NOT NULL DEFAULT 50,"
            "  deadline_at       INTEGER,"
            "  parent_id         INTEGER REFERENCES goals(id) ON DELETE SET NULL,"
            "  created_at        INTEGER NOT NULL,"
            "  last_progress_at  INTEGER NOT NULL,"
            "  blocked_reason    TEXT NOT NULL DEFAULT '',"
            "  plan_json         TEXT NOT NULL DEFAULT '',"
            "  tags_json         TEXT NOT NULL DEFAULT '[]'"
            ");",
            "create goals");
        execOrLog(db,
            "CREATE INDEX IF NOT EXISTS goals_status_idx   ON goals(status);"
            "CREATE INDEX IF NOT EXISTS goals_parent_idx   ON goals(parent_id);"
            "CREATE INDEX IF NOT EXISTS goals_deadline_idx ON goals(deadline_at);",
            "indexes");

        // Plain (non-external-content) FTS5 mirror over summary + blocked_reason.
        // External content + the 'delete' sentinel turned out brittle with our
        // trigger pattern; the storage cost of duplicating two short text
        // columns is negligible compared to the bug-surface savings.
        execOrLog(db,
            "CREATE VIRTUAL TABLE IF NOT EXISTS goals_fts USING fts5("
            "  summary, blocked_reason"
            ");",
            "fts5");

        // Triggers maintain the FTS5 mirror. UPDATE only fires for the columns
        // that actually feed the index, so bumpProgress / setPriority don't
        // pay the reindex cost.
        execOrLog(db,
            "CREATE TRIGGER IF NOT EXISTS goals_ai AFTER INSERT ON goals BEGIN "
            "  INSERT INTO goals_fts(rowid, summary, blocked_reason) "
            "    VALUES (new.id, new.summary, new.blocked_reason); "
            "END;",
            "trg ai");
        execOrLog(db,
            "CREATE TRIGGER IF NOT EXISTS goals_ad AFTER DELETE ON goals BEGIN "
            "  DELETE FROM goals_fts WHERE rowid = old.id; "
            "END;",
            "trg ad");
        execOrLog(db,
            "CREATE TRIGGER IF NOT EXISTS goals_au "
            "AFTER UPDATE OF summary, blocked_reason ON goals BEGIN "
            "  DELETE FROM goals_fts WHERE rowid = old.id; "
            "  INSERT INTO goals_fts(rowid, summary, blocked_reason) "
            "    VALUES (new.id, new.summary, new.blocked_reason); "
            "END;",
            "trg au");
    }

    void mirrorEpisodic(const Goal& g, GoalStatus prev, GoalStatus next,
                        const std::string& note) {
        if (!cfg.episodic_sink) return;
        EpisodicEvent ev;
        ev.kind = "goal_state";
        std::ostringstream os;
        os << "goal #" << g.id << " " << toString(prev) << " → "
           << toString(next) << " — " << g.summary;
        if (!note.empty()) os << " (" << note << ")";
        ev.summary = os.str();
        json p;
        p["goal_id"]  = g.id;
        p["summary"]  = g.summary;
        p["from"]     = toString(prev);
        p["to"]       = toString(next);
        if (!note.empty()) p["note"] = note;
        if (!g.tags.empty()) p["tags"] = g.tags;
        ev.payload_json = p.dump();

        // Salience floor: completions are notable, blocks moderate, others light.
        if (next == GoalStatus::Done)             ev.salience = 0.7;
        else if (next == GoalStatus::Blocked)     ev.salience = 0.5;
        else if (next == GoalStatus::Accepted)    ev.salience = 0.5;
        else if (next == GoalStatus::Cancelled
              || next == GoalStatus::Rejected)    ev.salience = 0.4;
        else                                      ev.salience = 0.3;

        cfg.episodic_sink->recordEvent(std::move(ev));
    }
};

// ─── ctor / dtor ───────────────────────────────────────────────────────────

GoalStore::GoalStore(Config cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = std::move(cfg);
    if (p_->cfg.db_path.empty()) {
        log::error("GoalStore: empty db_path");
        return;
    }
    if (sqlite3_open(p_->cfg.db_path.c_str(), &p_->db) != SQLITE_OK) {
        log::error("GoalStore: cannot open " + p_->cfg.db_path);
        if (p_->db) { sqlite3_close(p_->db); p_->db = nullptr; }
        return;
    }
    p_->initSchema();
}

GoalStore::~GoalStore() {
    if (p_ && p_->db) sqlite3_close(p_->db);
}

// ─── propose ───────────────────────────────────────────────────────────────

std::int64_t GoalStore::propose(Goal g) {
    if (!p_->db || g.summary.empty()) return 0;
    std::lock_guard<std::mutex> lk(p_->mu);

    long long now = nowEpoch();
    sqlite3_stmt* st = nullptr;
    const char* q =
        "INSERT INTO goals(summary, status, priority, deadline_at, parent_id, "
        "                  created_at, last_progress_at, blocked_reason, "
        "                  plan_json, tags_json) "
        "VALUES (?, 'proposed', ?, ?, ?, ?, ?, '', ?, ?)";
    if (sqlite3_prepare_v2(p_->db, q, -1, &st, nullptr) != SQLITE_OK) {
        log::error(std::string("GoalStore::propose prepare: ")
                   + sqlite3_errmsg(p_->db));
        return 0;
    }
    sqlite3_bind_text (st, 1, g.summary.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 2, g.priority);
    if (g.deadline_at)
        sqlite3_bind_int64(st, 3, duration_cast<seconds>(
                               g.deadline_at->time_since_epoch()).count());
    else
        sqlite3_bind_null (st, 3);
    if (g.parent_id) sqlite3_bind_int64(st, 4, *g.parent_id);
    else             sqlite3_bind_null (st, 4);
    sqlite3_bind_int64(st, 5, now);
    sqlite3_bind_int64(st, 6, now);
    sqlite3_bind_text (st, 7, g.plan_json.c_str(), -1, SQLITE_TRANSIENT);
    auto tags_blob = serialiseTags(g.tags);
    sqlite3_bind_text (st, 8, tags_blob.c_str(),   -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        log::error(std::string("GoalStore::propose step: ")
                   + sqlite3_errmsg(p_->db));
        return 0;
    }
    auto id = sqlite3_last_insert_rowid(p_->db);

    if (p_->cfg.episodic_sink) {
        Goal mirror = g;
        mirror.id     = id;
        mirror.status = GoalStatus::Proposed;
        mirror.created_at        = tpFromEpoch(now);
        mirror.last_progress_at  = tpFromEpoch(now);
        p_->mirrorEpisodic(mirror, GoalStatus::Proposed, GoalStatus::Proposed,
                           "proposed");
    }
    return id;
}

// ─── status transitions ────────────────────────────────────────────────────

bool GoalStore::setStatus(std::int64_t id, GoalStatus to, std::string note) {
    if (!p_->db) return false;
    std::lock_guard<std::mutex> lk(p_->mu);

    // Read current state.
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(p_->db,
            "SELECT status FROM goals WHERE id = ?", -1, &sel, nullptr)
            != SQLITE_OK) return false;
    sqlite3_bind_int64(sel, 1, id);
    GoalStatus from = GoalStatus::Proposed;
    bool found = false;
    if (sqlite3_step(sel) == SQLITE_ROW) {
        auto* s = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
        if (s) {
            if (auto cur = goalStatusFromString(s)) { from = *cur; found = true; }
        }
    }
    sqlite3_finalize(sel);
    if (!found) return false;
    if (!isValidTransition(from, to)) return false;
    if (from == to) return true;     // no-op, but report success

    // Update.
    sqlite3_stmt* upd = nullptr;
    const char* q =
        "UPDATE goals SET status = ?, last_progress_at = ?, "
        "                 blocked_reason = CASE WHEN ?='blocked' "
        "                                       THEN blocked_reason "
        "                                       ELSE '' END "
        "WHERE id = ?";
    if (sqlite3_prepare_v2(p_->db, q, -1, &upd, nullptr) != SQLITE_OK) return false;
    auto to_str = toString(to);
    long long now = nowEpoch();
    sqlite3_bind_text (upd, 1, to_str, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, now);
    sqlite3_bind_text (upd, 3, to_str, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 4, id);
    int rc = sqlite3_step(upd);
    sqlite3_finalize(upd);
    if (rc != SQLITE_DONE) return false;

    if (p_->cfg.episodic_sink) {
        // Re-fetch outside lock-aware path is OK — we still hold it.
        sqlite3_stmt* fst = nullptr;
        std::string sel2 = std::string("SELECT ") + kSelectCols
                         + " FROM goals g WHERE g.id = ?";
        Goal mirror;
        if (sqlite3_prepare_v2(p_->db, sel2.c_str(), -1, &fst, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(fst, 1, id);
            if (sqlite3_step(fst) == SQLITE_ROW) mirror = readRow(fst);
            sqlite3_finalize(fst);
        }
        p_->mirrorEpisodic(mirror, from, to, std::move(note));
    }
    return true;
}

bool GoalStore::block(std::int64_t id, std::string reason) {
    if (!p_->db) return false;

    // Need to set the reason in addition to status; do it in one pass.
    std::lock_guard<std::mutex> lk(p_->mu);

    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(p_->db,
            "SELECT status FROM goals WHERE id = ?", -1, &sel, nullptr)
            != SQLITE_OK) return false;
    sqlite3_bind_int64(sel, 1, id);
    GoalStatus from = GoalStatus::Proposed;
    bool found = false;
    if (sqlite3_step(sel) == SQLITE_ROW) {
        if (auto* s = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0))) {
            if (auto cur = goalStatusFromString(s)) { from = *cur; found = true; }
        }
    }
    sqlite3_finalize(sel);
    if (!found) return false;
    if (!isValidTransition(from, GoalStatus::Blocked)) return false;

    sqlite3_stmt* upd = nullptr;
    const char* q =
        "UPDATE goals SET status='blocked', blocked_reason=?, "
        "                 last_progress_at=? WHERE id=?";
    if (sqlite3_prepare_v2(p_->db, q, -1, &upd, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text (upd, 1, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, nowEpoch());
    sqlite3_bind_int64(upd, 3, id);
    int rc = sqlite3_step(upd);
    sqlite3_finalize(upd);
    if (rc != SQLITE_DONE) return false;

    if (p_->cfg.episodic_sink) {
        sqlite3_stmt* fst = nullptr;
        std::string sel2 = std::string("SELECT ") + kSelectCols
                         + " FROM goals g WHERE g.id = ?";
        Goal mirror;
        if (sqlite3_prepare_v2(p_->db, sel2.c_str(), -1, &fst, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(fst, 1, id);
            if (sqlite3_step(fst) == SQLITE_ROW) mirror = readRow(fst);
            sqlite3_finalize(fst);
        }
        p_->mirrorEpisodic(mirror, from, GoalStatus::Blocked, std::move(reason));
    }
    return true;
}

bool GoalStore::unblock(std::int64_t id, std::string note) {
    // Strict: only Blocked → InProgress. Header documents this contract so
    // CLI / scheduler code can use it as a precondition check.
    auto g = get(id);
    if (!g || g->status != GoalStatus::Blocked) return false;
    return setStatus(id, GoalStatus::InProgress, std::move(note));
}

// ─── field updates ─────────────────────────────────────────────────────────

namespace {

bool runUpdate1(sqlite3* db, std::mutex& mu,
                const char* sql,
                std::function<void(sqlite3_stmt*)> bind,
                std::int64_t id) {
    if (!db) return false;
    std::lock_guard<std::mutex> lk(mu);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    bind(st);
    sqlite3_bind_int64(st, sqlite3_bind_parameter_count(st), id);
    int rc = sqlite3_step(st);
    int changes = sqlite3_changes(db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && changes > 0;
}

} // namespace

bool GoalStore::setPlanJson(std::int64_t id, std::string plan_json) {
    return runUpdate1(p_->db, p_->mu,
        "UPDATE goals SET plan_json=?, last_progress_at=? WHERE id=?",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_text (st, 1, plan_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, nowEpoch());
        }, id);
}

bool GoalStore::setPriority(std::int64_t id, int priority) {
    return runUpdate1(p_->db, p_->mu,
        "UPDATE goals SET priority=? WHERE id=?",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_int(st, 1, priority);
        }, id);
}

bool GoalStore::setDeadline(std::int64_t id,
                            std::optional<GoalTimestamp> deadline_at) {
    return runUpdate1(p_->db, p_->mu,
        "UPDATE goals SET deadline_at=? WHERE id=?",
        [&](sqlite3_stmt* st) {
            if (deadline_at)
                sqlite3_bind_int64(st, 1, duration_cast<seconds>(
                                       deadline_at->time_since_epoch()).count());
            else
                sqlite3_bind_null (st, 1);
        }, id);
}

bool GoalStore::setSummary(std::int64_t id, std::string summary) {
    if (summary.empty()) return false;
    return runUpdate1(p_->db, p_->mu,
        "UPDATE goals SET summary=? WHERE id=?",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_text(st, 1, summary.c_str(), -1, SQLITE_TRANSIENT);
        }, id);
}

bool GoalStore::setTags(std::int64_t id, std::vector<std::string> tags) {
    auto blob = serialiseTags(tags);
    return runUpdate1(p_->db, p_->mu,
        "UPDATE goals SET tags_json=? WHERE id=?",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_text(st, 1, blob.c_str(), -1, SQLITE_TRANSIENT);
        }, id);
}

bool GoalStore::bumpProgress(std::int64_t id) {
    return runUpdate1(p_->db, p_->mu,
        "UPDATE goals SET last_progress_at=? WHERE id=?",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_int64(st, 1, nowEpoch());
        }, id);
}

// ─── reads ─────────────────────────────────────────────────────────────────

std::optional<Goal> GoalStore::get(std::int64_t id) const {
    if (!p_->db) return std::nullopt;
    std::lock_guard<std::mutex> lk(p_->mu);

    std::string q = std::string("SELECT ") + kSelectCols
                  + " FROM goals g WHERE g.id = ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, q.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_int64(st, 1, id);
    std::optional<Goal> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = readRow(st);
    sqlite3_finalize(st);
    return out;
}

std::vector<Goal> GoalStore::list(const GoalQuery& q) const {
    if (!p_->db) return {};
    std::lock_guard<std::mutex> lk(p_->mu);

    std::ostringstream sql;
    sql << "SELECT " << kSelectCols << " FROM goals g";
    bool joined_fts = false;

    auto fts_q = buildFtsQuery(q.text);
    if (!fts_q.empty()) {
        // No alias — FTS5 MATCH is happiest referenced by the original
        // virtual-table name. The JOIN binds it to the canonical goals row.
        sql << " JOIN goals_fts ON goals_fts.rowid = g.id";
        joined_fts = true;
    }
    sql << " WHERE 1=1";
    if (joined_fts)         sql << " AND goals_fts MATCH ?";
    if (q.status)           sql << " AND g.status = ?";
    if (q.parent_id)        sql << " AND g.parent_id = ?";
    if (!q.tag.empty())     sql << " AND g.tags_json LIKE ?";
    sql << (q.order_by_priority
              ? " ORDER BY g.priority DESC, g.last_progress_at DESC"
              : " ORDER BY g.last_progress_at DESC, g.id DESC");
    sql << " LIMIT ?";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, sql.str().c_str(), -1, &st, nullptr)
            != SQLITE_OK) {
        log::error(std::string("GoalStore::list prepare: ")
                   + sqlite3_errmsg(p_->db));
        return {};
    }

    int idx = 1;
    if (joined_fts)        sqlite3_bind_text (st, idx++, fts_q.c_str(), -1, SQLITE_TRANSIENT);
    if (q.status)          sqlite3_bind_text (st, idx++, toString(*q.status), -1, SQLITE_TRANSIENT);
    if (q.parent_id)       sqlite3_bind_int64(st, idx++, *q.parent_id);
    std::string tag_pat;
    if (!q.tag.empty()) {
        tag_pat = "%\"" + q.tag + "\"%";
        sqlite3_bind_text(st, idx++, tag_pat.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st, idx++, std::max(1, q.limit));

    std::vector<Goal> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readRow(st));
    sqlite3_finalize(st);
    return out;
}

std::vector<Goal> GoalStore::children(std::int64_t parent_id) const {
    GoalQuery q;
    q.parent_id = parent_id;
    q.limit     = 1000;
    return list(q);
}

std::vector<Goal> GoalStore::ancestors(std::int64_t id) const {
    if (!p_->db) return {};
    std::vector<Goal> chain;
    std::unordered_set<std::int64_t> seen;
    auto cur = get(id);
    if (!cur) return {};
    auto pid = cur->parent_id;
    while (pid) {
        if (!seen.insert(*pid).second) {
            log::warn("GoalStore::ancestors: cycle detected at id="
                      + std::to_string(*pid));
            break;
        }
        auto p = get(*pid);
        if (!p) break;
        chain.push_back(*p);
        pid = p->parent_id;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

std::vector<Goal> GoalStore::subtree(std::int64_t root_id) const {
    auto root = get(root_id);
    if (!root) return {};

    std::vector<Goal> out;
    std::unordered_set<std::int64_t> seen;
    std::queue<Goal> bfs;
    bfs.push(*root);
    seen.insert(root_id);

    while (!bfs.empty()) {
        Goal cur = std::move(bfs.front());
        bfs.pop();
        std::int64_t cur_id = cur.id;
        out.push_back(std::move(cur));
        for (auto& kid : children(cur_id)) {
            if (seen.insert(kid.id).second) bfs.push(std::move(kid));
        }
    }
    return out;
}

std::vector<Goal> GoalStore::dueSoon(std::chrono::seconds horizon) const {
    if (!p_->db) return {};
    std::lock_guard<std::mutex> lk(p_->mu);
    std::string sql = std::string("SELECT ") + kSelectCols
        + " FROM goals g "
          " WHERE g.deadline_at IS NOT NULL "
          "   AND g.deadline_at <= ? "
          "   AND g.status NOT IN ('done','rejected','cancelled') "
          " ORDER BY g.deadline_at ASC";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return {};
    sqlite3_bind_int64(st, 1, nowEpoch() + horizon.count());
    std::vector<Goal> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readRow(st));
    sqlite3_finalize(st);
    return out;
}

std::vector<Goal> GoalStore::staleInProgress(std::chrono::seconds threshold) const {
    if (!p_->db) return {};
    std::lock_guard<std::mutex> lk(p_->mu);
    std::string sql = std::string("SELECT ") + kSelectCols
        + " FROM goals g "
          " WHERE g.status = 'in_progress' "
          "   AND g.last_progress_at < ? "
          " ORDER BY g.last_progress_at ASC";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return {};
    sqlite3_bind_int64(st, 1, nowEpoch() - threshold.count());
    std::vector<Goal> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readRow(st));
    sqlite3_finalize(st);
    return out;
}

std::vector<Goal> GoalStore::blocked() const {
    GoalQuery q;
    q.status = GoalStatus::Blocked;
    q.limit  = 1000;
    return list(q);
}

int GoalStore::countByStatus(GoalStatus s) const {
    if (!p_->db) return 0;
    std::lock_guard<std::mutex> lk(p_->mu);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db,
            "SELECT COUNT(*) FROM goals WHERE status = ?",
            -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, toString(s), -1, SQLITE_TRANSIENT);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

int GoalStore::totalCount() const {
    if (!p_->db) return 0;
    std::lock_guard<std::mutex> lk(p_->mu);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, "SELECT COUNT(*) FROM goals", -1, &st, nullptr)
            != SQLITE_OK) return 0;
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

} // namespace arise
