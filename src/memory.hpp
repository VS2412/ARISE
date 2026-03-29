#pragma once
#include <string>
#include <vector>
#include <sqlite3.h>

struct MemoryEntry {
    std::string role;
    std::string content;
    std::string timestamp;
};

class Memory {
public:
    explicit Memory(const std::string& dbPath);
    ~Memory();

    void save(const std::string& role, const std::string& content);
    void setFact(const std::string& key, const std::string& value);
    std::string getFact(const std::string& key) const;
    std::vector<MemoryEntry> getRecent(int n = 10) const;
    std::string getSummary() const; // for LLM context injection

private:
    sqlite3* db_{nullptr};
    void exec(const std::string& sql);
};