#include "memory.hpp"
#include "logger.hpp"
#include <stdexcept>
#include <algorithm>
#include <sstream>

Memory::Memory(const std::string& dbPath) {
    if (sqlite3_open(dbPath.c_str(), &db_) != SQLITE_OK) {
        Logger::error("Memory: cannot open DB: " + dbPath);
        db_ = nullptr;
        return;
    }
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
    )");
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
    const char* q = "INSERT OR REPLACE INTO user_facts (key, value) VALUES (?, ?)";
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
    const char* q = "SELECT role, content, timestamp FROM conversations ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, n);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MemoryEntry e;
            e.role      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            e.content   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            e.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            entries.push_back(e);
        }
        sqlite3_finalize(stmt);
    }
    std::reverse(entries.begin(), entries.end());
    return entries;
}

std::string Memory::getSummary() const {
    if (!db_) return "";
    std::ostringstream out;

    // known facts about the user
    sqlite3_stmt* stmt;
    const char* q1 = "SELECT key, value FROM user_facts ORDER BY updated_at DESC LIMIT 10";
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
    return out.str();
}