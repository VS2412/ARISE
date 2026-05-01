#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace arise {

using Timestamp = std::chrono::system_clock::time_point;

enum class MemoryType { Episodic, Semantic, Procedural, Preference };

// "What happened" — a single observable moment, scored by salience and
// possibly tagged with the mood snapshot at write time. Embedding is
// derived lazily from `summary` if absent.
struct EpisodicEvent {
    int64_t                 id = 0;
    std::string             kind;             // "conversation_turn" | "screen_obs" | ...
    std::string             summary;          // human-readable
    std::string             payload_json;     // optional source-specific blob
    double                  salience = 0.0;   // 0..1; auto-heuristic if 0 at write
    std::string             mood_at;          // mood label captured at the moment
    std::vector<int64_t>    refs;             // links to other episodic ids
    std::optional<Timestamp> decay_at;        // null = permanent
    std::vector<float>      embedding;        // populated lazily; not user-facing
    Timestamp               ts;               // auto-stamped on record if zero
};

// "What is true." Subject/predicate/object triples with confidence so a
// later contradiction can win on weight rather than recency alone.
struct SemanticFact {
    int64_t                  id = 0;
    std::string              subject;
    std::string              predicate;
    std::string              object;
    double                   confidence = 1.0;
    std::optional<int64_t>   source_episodic_id;
    Timestamp                last_confirmed;
};

// "How I do things." Indexed by goal pattern; success/failure counts let
// the planner prefer recipes that have actually worked.
struct Procedure {
    int64_t      id = 0;
    std::string  goal_pattern;
    std::string  steps_json;
    int          success_count = 0;
    int          failure_count = 0;
    Timestamp    last_used;
};

// "How the user is." Stable, distilled — distinct from semantic facts
// because preferences tune behaviour rather than describe state.
struct Preference {
    std::string  key;
    std::string  value;
    double       weight = 1.0;
    Timestamp    last_updated;
};

// Affect state. Two-axis valence/arousal, both clamped to [-1, 1] so
// decay toward baseline is a simple exponential pull to zero.
struct Mood {
    std::string  baseline = "neutral";
    std::string  current  = "neutral";
    double       valence  = 0.0;     // -1..+1
    double       arousal  = 0.0;     // -1..+1
    Timestamp    last_change_at;
};

struct RecallQuery {
    std::string                   text;
    std::vector<MemoryType>       types{MemoryType::Episodic, MemoryType::Semantic};
    int                           limit = 8;
};

// Result of a hybrid recall. `rendered` is the single human-readable line
// the prompt-builder should quote — keeps "you sounded frustrated about X"
// formatting in one place.
struct RecallHit {
    MemoryType                   type;
    std::string                  rendered;
    double                       score = 0.0;
    Timestamp                    when;
    std::optional<EpisodicEvent> episodic;
    std::optional<SemanticFact>  fact;
};

// Five SQLite databases + a JSON mood file behind a single facade. Owns
// its own embedding client and a background decay thread. Safe to share
// across threads — every public method takes the appropriate db mutex
// internally.
class MemoryCortex {
public:
    struct Config {
        std::string  root;                         // <ariseRoot>/memory
        std::string  ollama_url   = "http://127.0.0.1:11434";
        std::string  embed_model  = "nomic-embed-text";
        int          embed_dim    = 768;
        std::string  sqlite_vec_path;              // path to vec0.so (empty = FTS-only)
        std::string  embed_cache_path;             // empty = no cache db
        bool         decay_thread = true;
        int          decay_check_interval_sec = 600;       // 10 min
        double       mood_half_life_seconds   = 8 * 3600.0; // 8h
    };

    explicit MemoryCortex(Config cfg);
    ~MemoryCortex();
    MemoryCortex(const MemoryCortex&)            = delete;
    MemoryCortex& operator=(const MemoryCortex&) = delete;

    // Episodic
    int64_t                         recordEvent(EpisodicEvent ev);
    std::optional<EpisodicEvent>    getEvent(int64_t id) const;
    std::vector<EpisodicEvent>      recentEvents(int n = 20) const;
    int                             purgeDecayed();

    // Semantic — contradiction-aware upsert
    int64_t                         upsertFact(SemanticFact fact);
    std::vector<SemanticFact>       queryFacts(const std::string& subject,
                                               const std::string& predicate = "") const;

    // Procedural
    int64_t                         recordProcedure(Procedure p);
    std::vector<Procedure>          matchProcedures(const std::string& goal_pattern,
                                                    int n = 3) const;
    void                            bumpProcedure(int64_t id, bool succeeded);

    // Preferences
    void                            setPreference(Preference p);
    std::optional<Preference>       getPreference(const std::string& key) const;
    std::vector<Preference>         listPreferences() const;

    // Mood
    Mood                            mood() const;
    void                            setMoodBaseline(const std::string& label);
    void                            nudgeMood(double dValence, double dArousal,
                                              std::string label = "");
    void                            tickMoodDecay();

    // Hybrid recall (FTS5 + vec0 + recency, RRF-blended).
    std::vector<RecallHit>          recall(const RecallQuery& q) const;

    // Maintenance / introspection.
    void                            compact();
    int64_t                         totalSizeBytes() const;
    bool                            vecEnabled() const;

private:
    struct Impl;
    std::unique_ptr<Impl>  p_;
    std::thread            decayThread_;
    void                   decayLoopFn_();
};

const char* toString(MemoryType t);

} // namespace arise
