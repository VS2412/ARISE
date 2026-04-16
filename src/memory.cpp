#include "memory.hpp"
#include "logger.hpp"
#include <algorithm>
#include <sstream>
#include <cctype>

Memory::Memory(const std::string& dbPath) {
    if (sqlite3_open(dbPath.c_str(), &db_) != SQLITE_OK) {
        Logger::error("Memory: cannot open DB: " + dbPath);
        db_ = nullptr;
        return;
    }
    // Core tables
    exec(R"(
        CREATE TABLE IF NOT EXISTS conversations (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT DEFAULT (datetime('now','localtime')),
            role      TEXT NOT NULL,
            content   TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS user_facts (
            key        TEXT PRIMARY KEY,
            value      TEXT NOT NULL,
            updated_at TEXT DEFAULT (datetime('now','localtime'))
        );
        CREATE TABLE IF NOT EXISTS summaries (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp  TEXT DEFAULT (datetime('now','localtime')),
            content    TEXT NOT NULL,
            turn_start INTEGER,
            turn_end   INTEGER
        );
    )");

    // FTS5 virtual table synced with conversations
    exec(R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS conversations_fts USING fts5(
            content, content='conversations', content_rowid='id'
        );
        CREATE TRIGGER IF NOT EXISTS conversations_ai AFTER INSERT ON conversations BEGIN
            INSERT INTO conversations_fts(rowid, content) VALUES (new.id, new.content);
        END;
        CREATE TRIGGER IF NOT EXISTS conversations_ad AFTER DELETE ON conversations BEGIN
            INSERT INTO conversations_fts(conversations_fts, rowid, content)
                VALUES('delete', old.id, old.content);
        END;
        CREATE TRIGGER IF NOT EXISTS conversations_au AFTER UPDATE ON conversations BEGIN
            INSERT INTO conversations_fts(conversations_fts, rowid, content)
                VALUES('delete', old.id, old.content);
            INSERT INTO conversations_fts(rowid, content) VALUES (new.id, new.content);
        END;
    )");

    // One-time backfill: rebuild FTS index from conversations if the index is empty.
    // (External-content FTS NOT-EXISTS check is unreliable, so check docsize directly.)
    {
        sqlite3_stmt* stmt;
        int indexed = 0;
        if (sqlite3_prepare_v2(db_,
                "SELECT COUNT(*) FROM conversations_fts_docsize",
                -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) indexed = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        int total = 0;
        if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM conversations",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        if (indexed < total) {
            exec("INSERT INTO conversations_fts(conversations_fts) VALUES('rebuild');");
            Logger::info("Memory: rebuilt FTS index (" + std::to_string(total) + " rows)");
        }
    }

    Logger::info("Memory: loaded from " + dbPath);
}

Memory::~Memory() {
    if (db_) sqlite3_close(db_);
}

void Memory::exec(const std::string& sql) {
    if (!db_) return;
    char* err = nullptr;
    sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (err) { Logger::error("Memory SQL: " + std::string(err)); sqlite3_free(err); }
}

void Memory::save(const std::string& role, const std::string& content) {
    if (!db_) return;
    sqlite3_stmt* stmt;
    const char* q = "INSERT INTO conversations (role, content) VALUES (?, ?)";
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, role.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Memory::setFact(const std::string& key, const std::string& value) {
    if (!db_) return;
    sqlite3_stmt* stmt;
    const char* q = "INSERT OR REPLACE INTO user_facts (key, value, updated_at) "
                    "VALUES (?, ?, datetime('now','localtime'))";
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::string Memory::getFact(const std::string& key) const {
    if (!db_) return "";
    sqlite3_stmt* stmt;
    const char* q = "SELECT value FROM user_facts WHERE key = ?";
    std::string result;
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        sqlite3_finalize(stmt);
    }
    return result;
}

std::vector<MemoryEntry> Memory::getRecent(int n) const {
    std::vector<MemoryEntry> entries;
    if (!db_) return entries;
    sqlite3_stmt* stmt;
    const char* q = "SELECT id, role, content, timestamp FROM conversations "
                    "ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, n);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MemoryEntry e;
            e.id        = sqlite3_column_int(stmt, 0);
            e.role      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            e.content   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            e.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            entries.push_back(e);
        }
        sqlite3_finalize(stmt);
    }
    std::reverse(entries.begin(), entries.end());
    return entries;
}

// ─── FTS5 search ───
std::vector<MemoryEntry> Memory::search(const std::string& query, int limit) const {
    std::vector<MemoryEntry> results;
    if (!db_ || query.empty()) return results;

    // Build safe FTS5 query — quote each alphanumeric token
    std::string ftsQuery;
    std::istringstream iss(query);
    std::string token;
    while (iss >> token) {
        std::string safe;
        for (char c : token) if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') safe += c;
        if (safe.empty()) continue;
        if (!ftsQuery.empty()) ftsQuery += " OR ";
        ftsQuery += "\"" + safe + "\"";
    }
    if (ftsQuery.empty()) return results;

    sqlite3_stmt* stmt;
    const char* q =
        "SELECT c.id, c.role, c.content, c.timestamp "
        "FROM conversations c JOIN conversations_fts f ON c.id = f.rowid "
        "WHERE conversations_fts MATCH ? "
        "ORDER BY rank LIMIT ?";
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ftsQuery.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 2, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MemoryEntry e;
            e.id        = sqlite3_column_int(stmt, 0);
            e.role      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            e.content   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            e.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            results.push_back(e);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

// ─── Summaries ───
int Memory::lastConversationId() const {
    if (!db_) return 0;
    sqlite3_stmt* stmt;
    int maxId = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(id),0) FROM conversations",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) maxId = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return maxId;
}

int Memory::lastSummarizedId() const {
    if (!db_) return 0;
    sqlite3_stmt* stmt;
    int lastEnd = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(turn_end),0) FROM summaries",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) lastEnd = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return lastEnd;
}

void Memory::saveSummary(const std::string& content, int turnStart, int turnEnd) {
    if (!db_) return;
    sqlite3_stmt* stmt;
    const char* q = "INSERT INTO summaries (content, turn_start, turn_end) VALUES (?, ?, ?)";
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 2, turnStart);
        sqlite3_bind_int (stmt, 3, turnEnd);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<SummaryEntry> Memory::getSummaries(int n) const {
    std::vector<SummaryEntry> results;
    if (!db_) return results;
    sqlite3_stmt* stmt;
    const char* q = "SELECT content, timestamp, turn_start, turn_end FROM summaries "
                    "ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, n);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SummaryEntry e;
            e.content   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            e.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            e.turnStart = sqlite3_column_int(stmt, 2);
            e.turnEnd   = sqlite3_column_int(stmt, 3);
            results.push_back(e);
        }
        sqlite3_finalize(stmt);
    }
    std::reverse(results.begin(), results.end()); // oldest first
    return results;
}

std::vector<MemoryEntry> Memory::getRange(int minIdExclusive, int maxIdInclusive) const {
    std::vector<MemoryEntry> entries;
    if (!db_) return entries;
    sqlite3_stmt* stmt;
    const char* q = "SELECT id, role, content, timestamp FROM conversations "
                    "WHERE id > ? AND id <= ? ORDER BY id ASC";
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, minIdExclusive);
        sqlite3_bind_int(stmt, 2, maxIdInclusive);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MemoryEntry e;
            e.id        = sqlite3_column_int(stmt, 0);
            e.role      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            e.content   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            e.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            entries.push_back(e);
        }
        sqlite3_finalize(stmt);
    }
    return entries;
}

std::string Memory::getSummary() const {
    if (!db_) return "";
    std::ostringstream out;

    // Facts about the user — persistent memory
    sqlite3_stmt* stmt;
    const char* q1 = "SELECT key, value FROM user_facts ORDER BY updated_at DESC LIMIT 15";
    if (sqlite3_prepare_v2(db_, q1, -1, &stmt, nullptr) == SQLITE_OK) {
        bool any = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!any) { out << "Known about user: "; any = true; }
            out << sqlite3_column_text(stmt, 0) << "="
                << sqlite3_column_text(stmt, 1) << "; ";
        }
        sqlite3_finalize(stmt);
        if (any) out << "\n";
    }

    // Older conversation summaries — long-term memory window
    auto summaries = getSummaries(3);
    if (!summaries.empty()) {
        out << "Earlier conversations:\n";
        for (const auto& s : summaries)
            out << "[" << s.timestamp << "] " << s.content << "\n";
    }

    // Deliberately NOT dumping recent raw turns here — the LLM maintains its
    // own rolling message history inside the chat request, and re-injecting
    // the DB copy was leaking pre-Phase-7 persona breakage ("I'm Qwen", "I
    // don't have access") back into the system prompt and causing the model
    // to mimic its own older bad output. Facts + summaries are enough.

    return out.str();
}
