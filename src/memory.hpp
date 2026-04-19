#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
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

// Phase 8: hybrid-search candidate wrapper. `source` is "fts", "vec", or "both".
struct SearchResult {
    MemoryEntry entry;
    double score   = 0.0;   // combined RRF + recency score (higher = better)
    double vecSim  = 0.0;   // cosine similarity in [-1, 1] (0 if no vec match)
    double fts     = 0.0;   // normalized FTS rank (0 if no fts match)
    std::string source;     // "fts" | "vec" | "both"
};

struct EntityRow {
    int id = 0;
    std::string name;
    std::string type;
    std::string firstSeen;
    std::string lastSeen;
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

    // Phase 3: FTS5 search (kept as fast keyword fallback)
    std::vector<MemoryEntry> search(const std::string& query, int limit = 5) const;

    // Phase 3: Summaries (long-term memory)
    int  lastConversationId() const;
    int  lastSummarizedId()   const;
    void saveSummary(const std::string& content, int turnStart, int turnEnd);
    std::vector<SummaryEntry> getSummaries(int n = 3) const;
    std::vector<MemoryEntry>  getRange(int minIdExclusive, int maxIdInclusive) const;

    // ─── Phase 8: embeddings + hybrid retrieval ───
    bool vecEnabled() const { return vecEnabled_; }

    // Calls Ollama /api/embeddings; returns empty vec on failure.
    // Caller gets a unit-normalized vector (caller can skip normalization).
    std::vector<float> embed(const std::string& text) const;

    // Union of FTS5 top-K and vec top-K, reranked by
    //   score = RRF(rank_fts, rank_vec) + recency_weight * decay(age)
    std::vector<SearchResult> hybridSearch(const std::string& query, int limit = 5) const;

    // ─── Phase 8: entity graph ───
    int  upsertEntity(const std::string& name, const std::string& type);
    void addEntityRelation(int entity1Id, const std::string& relation,
                           int entity2Id, const std::string& context);
    std::vector<EntityRow> listEntities(const std::string& type = "", int limit = 20) const;

private:
    sqlite3* db_{nullptr};
    mutable std::mutex dbMutex_;          // guards multi-statement ops from worker threads
    bool vecEnabled_{false};
    int  embedDim_{768};
    std::string embedModel_;
    std::string ollamaUrl_;
    std::atomic<bool> stopping_{false};
    std::thread backfillThread_;

    void exec(const std::string& sql);
    void loadVecExtension(const std::string& path);
    void ensureVecSchema();

    // Compute embedding and insert into conversations_vec / summary_vec.
    // Silent on failure — vec is best-effort, FTS/raw remains authoritative.
    void storeConversationVec(long long rowid, const std::string& content);
    void storeSummaryVec(long long rowid, const std::string& content);

    // Background: walk conversations/summaries lacking a vec row, embed, insert.
    void backfillLoop();
};
