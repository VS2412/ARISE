#pragma once
#include <string>
#include <vector>
#include <sqlite3.h>

struct MemoryEntry {
    int id = 0;
    std::string role;
    std::string content;
    std::string timestamp;
};

struct SummaryEntry {
    std::string content;
    std::string timestamp;
    int turnStart = 0;
    int turnEnd   = 0;
};

class Memory {
public:
    explicit Memory(const std::string& dbPath);
    ~Memory();

    // Core
    void save(const std::string& role, const std::string& content);
    void setFact(const std::string& key, const std::string& value);
    std::string getFact(const std::string& key) const;
    std::vector<MemoryEntry> getRecent(int n = 10) const;
    std::string getSummary() const; // for LLM context injection

    // Phase 3: FTS5 search
    std::vector<MemoryEntry> search(const std::string& query, int limit = 5) const;

    // Phase 3: Summaries (long-term memory)
    int  lastConversationId() const;
    int  lastSummarizedId()   const;
    void saveSummary(const std::string& content, int turnStart, int turnEnd);
    std::vector<SummaryEntry> getSummaries(int n = 3) const;
    std::vector<MemoryEntry>  getRange(int minIdExclusive, int maxIdInclusive) const;

private:
    sqlite3* db_{nullptr};
    void exec(const std::string& sql);
};
