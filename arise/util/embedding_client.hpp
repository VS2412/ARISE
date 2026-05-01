#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <sqlite3.h>

namespace arise {

// Thin client for Ollama's /api/embeddings, plus an on-disk cache so the
// same text never costs two round-trips. Vectors are L2-normalized so
// downstream consumers (sqlite-vec, cosine sims) don't have to renormalize.
//
// Cache layout: a single-table SQLite DB keyed by (model, fnv64(text), len).
// fnv64 alone is ample for this scale; bundling the length further makes
// accidental collisions a non-issue. Hash + length keeps cache rows tiny.
class EmbeddingClient {
public:
    struct Config {
        std::string ollama_url           = "http://127.0.0.1:11434";
        std::string model                = "nomic-embed-text";
        int         dim                  = 768;
        std::string cache_path;             // empty = skip cache
        int         timeout_sec          = 10;
        int         connect_timeout_sec  = 2;
    };

    explicit EmbeddingClient(Config cfg);
    ~EmbeddingClient();
    EmbeddingClient(const EmbeddingClient&)            = delete;
    EmbeddingClient& operator=(const EmbeddingClient&) = delete;

    // Returns L2-normalized vector. Empty on any error (network, parse, dim).
    std::vector<float> embed(const std::string& text);

    int                dim()   const { return cfg_.dim; }
    const std::string& model() const { return cfg_.model; }

    // True if at least one round-trip to Ollama has succeeded since startup.
    // Cheap probe used by tests to GTEST_SKIP when offline.
    bool probe();

    // For tests / monitoring: count of cache rows.
    int  cacheSize() const;

private:
    Config              cfg_;
    sqlite3*            cache_{nullptr};
    mutable std::mutex  cache_mu_;
    std::atomic<bool>   reachable_{false};

    std::vector<float>  embedRemote_(const std::string& text);
    bool                cacheGet_(const std::string& key, std::vector<float>& out);
    void                cachePut_(const std::string& key, const std::vector<float>& v);
    static std::string  cacheKey_(const std::string& model, const std::string& text);
};

} // namespace arise
