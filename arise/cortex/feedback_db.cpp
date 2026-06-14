#include "cortex/feedback_db.hpp"

#include "util/log.hpp"

#include <chrono>
#include <mutex>
#include <sstream>

#include <sqlite3.h>

using namespace std::chrono;

namespace arise {

const char* decisionToString(Decision d) {
    switch (d) {
        case Decision::Pending:  return "pending";
        case Decision::Accepted: return "accepted";
        case Decision::Rejected: return "rejected";
        case Decision::Ignored:  return "ignored";
        case Decision::Timedout: return "timedout";
    }
    return "pending";
}

std::optional<Decision> decisionFromString(std::string_view s) {
    if (s == "pending")  return Decision::Pending;
    if (s == "accepted") return Decision::Accepted;
    if (s == "rejected") return Decision::Rejected;
    if (s == "ignored")  return Decision::Ignored;
    if (s == "timedout") return Decision::Timedout;
    return std::nullopt;
}

namespace {

long long nowEpoch() {
    return duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();
}

system_clock::time_point tpFromEpoch(long long e) {
    return system_clock::time_point(seconds(e));
}

void execOrLog(sqlite3* db, const char* sql, const char* what) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        log::error(std::string("FeedbackDb SQL ") + what + ": "
                   + (err ? err : "?"));
        if (err) sqlite3_free(err);
    }
}

constexpr const char* kSelectCols =
    "id, tier, category, text, source_topic, source_payload, "
    "proposed_at, decision, decided_at";

SuggestionRow readRow(sqlite3_stmt* st) {
    SuggestionRow r;
    r.id = sqlite3_column_int64(st, 0);
    if (auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 1))) {
        if (auto t = tierFromString(p)) r.tier = *t;
    }
    if (auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 2))) {
        r.category = p;
    }
    if (auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 3))) {
        r.text = p;
    }
    if (auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 4))) {
        r.source_topic = p;
    }
    if (auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 5))) {
        r.source_payload_json = p;
    }
    r.proposed_at = tpFromEpoch(sqlite3_column_int64(st, 6));
    if (auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 7))) {
        if (auto d = decisionFromString(p)) r.decision = *d;
    }
    if (sqlite3_column_type(st, 8) != SQLITE_NULL) {
        r.decided_at = tpFromEpoch(sqlite3_column_int64(st, 8));
    }
    return r;
}

} // namespace

struct FeedbackDb::Impl {
    Config              cfg;
    sqlite3*            db = nullptr;
    mutable std::mutex  mu;

    void initSchema() {
        execOrLog(db, "PRAGMA journal_mode = WAL;", "wal");
        execOrLog(db,
            "CREATE TABLE IF NOT EXISTS suggestions ("
            "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  tier            TEXT NOT NULL,"
            "  category        TEXT NOT NULL,"
            "  text            TEXT NOT NULL,"
            "  source_topic    TEXT NOT NULL DEFAULT '',"
            "  source_payload  TEXT NOT NULL DEFAULT '',"
            "  proposed_at     INTEGER NOT NULL,"
            "  decision        TEXT NOT NULL DEFAULT 'pending',"
            "  decided_at      INTEGER"
            ");",
            "create");
        execOrLog(db,
            "CREATE INDEX IF NOT EXISTS suggestions_cat_idx ON suggestions(category);"
            "CREATE INDEX IF NOT EXISTS suggestions_dec_idx ON suggestions(decision);"
            "CREATE INDEX IF NOT EXISTS suggestions_proposed_idx ON suggestions(proposed_at);",
            "indexes");
    }
};

FeedbackDb::FeedbackDb(Config cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = std::move(cfg);
    if (p_->cfg.db_path.empty()) {
        log::error("FeedbackDb: empty db_path");
        return;
    }
    if (sqlite3_open(p_->cfg.db_path.c_str(), &p_->db) != SQLITE_OK) {
        log::error("FeedbackDb: cannot open " + p_->cfg.db_path);
        if (p_->db) { sqlite3_close(p_->db); p_->db = nullptr; }
        return;
    }
    p_->initSchema();
}

FeedbackDb::~FeedbackDb() {
    if (p_ && p_->db) sqlite3_close(p_->db);
}

std::int64_t FeedbackDb::recordProposed(Suggestion& s) {
    if (!p_->db) return 0;
    std::lock_guard<std::mutex> lk(p_->mu);

    long long now = nowEpoch();
    if (s.proposed_at.time_since_epoch().count() == 0) {
        s.proposed_at = tpFromEpoch(now);
    } else {
        now = duration_cast<seconds>(s.proposed_at.time_since_epoch()).count();
    }

    sqlite3_stmt* st = nullptr;
    const char* q =
        "INSERT INTO suggestions(tier, category, text, source_topic, "
        "                         source_payload, proposed_at) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(p_->db, q, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, tierToString(s.tier),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, s.category.c_str(),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s.text.c_str(),             -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, s.source_topic.c_str(),     -1, SQLITE_TRANSIENT);
    auto blob = s.source_payload.is_null() ? std::string{} : s.source_payload.dump();
    sqlite3_bind_text(st, 5, blob.c_str(),               -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return 0;
    s.id = sqlite3_last_insert_rowid(p_->db);
    return s.id;
}

bool FeedbackDb::recordDecision(std::int64_t id, Decision d) {
    if (!p_->db || id <= 0) return false;
    std::lock_guard<std::mutex> lk(p_->mu);

    // Refuse to overwrite a terminal decision (idempotent re-Pending is fine).
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(p_->db,
            "SELECT decision FROM suggestions WHERE id = ?", -1, &sel, nullptr)
            != SQLITE_OK) return false;
    sqlite3_bind_int64(sel, 1, id);
    Decision cur = Decision::Pending;
    bool found = false;
    if (sqlite3_step(sel) == SQLITE_ROW) {
        if (auto* p = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0))) {
            if (auto cd = decisionFromString(p)) { cur = *cd; found = true; }
        }
    }
    sqlite3_finalize(sel);
    if (!found) return false;
    if (cur != Decision::Pending && cur != d) return false;

    sqlite3_stmt* upd = nullptr;
    const char* q = "UPDATE suggestions SET decision = ?, "
                    "                       decided_at = ? "
                    "WHERE id = ?";
    if (sqlite3_prepare_v2(p_->db, q, -1, &upd, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text (upd, 1, decisionToString(d), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, nowEpoch());
    sqlite3_bind_int64(upd, 3, id);
    int rc = sqlite3_step(upd);
    sqlite3_finalize(upd);
    return rc == SQLITE_DONE;
}

std::optional<SuggestionRow> FeedbackDb::get(std::int64_t id) const {
    if (!p_->db || id <= 0) return std::nullopt;
    std::lock_guard<std::mutex> lk(p_->mu);
    std::string q = std::string("SELECT ") + kSelectCols
                  + " FROM suggestions WHERE id = ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, q.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_int64(st, 1, id);
    std::optional<SuggestionRow> r;
    if (sqlite3_step(st) == SQLITE_ROW) r = readRow(st);
    sqlite3_finalize(st);
    return r;
}

std::vector<SuggestionRow>
FeedbackDb::list(const FeedbackQuery& q) const {
    if (!p_->db) return {};
    std::lock_guard<std::mutex> lk(p_->mu);

    std::ostringstream sql;
    sql << "SELECT " << kSelectCols << " FROM suggestions WHERE 1=1";
    if (q.decision)        sql << " AND decision = ?";
    if (q.tier)            sql << " AND tier = ?";
    if (!q.category.empty()) sql << " AND category = ?";
    sql << " ORDER BY proposed_at DESC, id DESC LIMIT ?";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, sql.str().c_str(), -1, &st, nullptr)
            != SQLITE_OK) return {};
    int idx = 1;
    if (q.decision) sqlite3_bind_text(st, idx++, decisionToString(*q.decision),
                                       -1, SQLITE_TRANSIENT);
    if (q.tier)     sqlite3_bind_text(st, idx++, tierToString(*q.tier),
                                       -1, SQLITE_TRANSIENT);
    if (!q.category.empty()) sqlite3_bind_text(st, idx++, q.category.c_str(),
                                                -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, idx++, std::max(1, q.limit));

    std::vector<SuggestionRow> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readRow(st));
    sqlite3_finalize(st);
    return out;
}

int FeedbackDb::consecutiveRejects(const std::string& category) const {
    if (!p_->db) return 0;
    std::lock_guard<std::mutex> lk(p_->mu);
    sqlite3_stmt* st = nullptr;
    // Walk most-recent-first; count rejects until any non-reject (excluding
    // pending — pending doesn't reset the streak, it just isn't decided).
    const char* q =
        "SELECT decision FROM suggestions WHERE category = ? "
        "ORDER BY proposed_at DESC, id DESC LIMIT 50";
    if (sqlite3_prepare_v2(p_->db, q, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, category.c_str(), -1, SQLITE_TRANSIENT);
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        auto* d = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        if (!d) continue;
        std::string ds = d;
        if (ds == "rejected") { ++n; continue; }
        if (ds == "pending")  { continue; }
        break;
    }
    sqlite3_finalize(st);
    return n;
}

int FeedbackDb::categoryCount(const std::string& category, Decision d) const {
    if (!p_->db) return 0;
    std::lock_guard<std::mutex> lk(p_->mu);
    sqlite3_stmt* st = nullptr;
    const char* q = "SELECT COUNT(*) FROM suggestions "
                    "WHERE category = ? AND decision = ?";
    if (sqlite3_prepare_v2(p_->db, q, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, category.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, decisionToString(d),    -1, SQLITE_TRANSIENT);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

int FeedbackDb::timeoutPending(std::chrono::seconds older_than) {
    if (!p_->db) return 0;
    std::lock_guard<std::mutex> lk(p_->mu);
    long long cutoff = nowEpoch() - older_than.count();
    sqlite3_stmt* st = nullptr;
    const char* q = "UPDATE suggestions "
                    "SET decision='timedout', decided_at=? "
                    "WHERE decision='pending' AND proposed_at < ?";
    if (sqlite3_prepare_v2(p_->db, q, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, nowEpoch());
    sqlite3_bind_int64(st, 2, cutoff);
    int rc = sqlite3_step(st);
    int n  = sqlite3_changes(p_->db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? n : 0;
}

} // namespace arise
