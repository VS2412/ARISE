#include "util/embedding_client.hpp"
#include "util/log.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace arise {

namespace {

size_t writeCb(void* ptr, size_t sz, size_t nm, std::string* out) {
    out->append(static_cast<char*>(ptr), sz * nm);
    return sz * nm;
}

void l2norm(std::vector<float>& v) {
    double s = 0.0;
    for (float x : v) s += double(x) * double(x);
    double n = std::sqrt(s);
    if (n <= 1e-12) return;
    float inv = static_cast<float>(1.0 / n);
    for (float& x : v) x *= inv;
}

uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

} // namespace

std::string EmbeddingClient::cacheKey_(const std::string& model, const std::string& text) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s:%016llx:%zu",
                  model.c_str(),
                  static_cast<unsigned long long>(fnv1a64(text)),
                  text.size());
    return buf;
}

EmbeddingClient::EmbeddingClient(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.cache_path.empty()) return;

    if (sqlite3_open(cfg_.cache_path.c_str(), &cache_) != SQLITE_OK) {
        log::warn("EmbeddingClient: cannot open cache db at " + cfg_.cache_path);
        if (cache_) { sqlite3_close(cache_); cache_ = nullptr; }
        return;
    }
    char* err = nullptr;
    const char* sql =
        "CREATE TABLE IF NOT EXISTS embed_cache ("
        "  key   TEXT PRIMARY KEY,"
        "  model TEXT NOT NULL,"
        "  dim   INTEGER NOT NULL,"
        "  vec   BLOB NOT NULL,"
        "  ts    TEXT DEFAULT (datetime('now','localtime'))"
        ");";
    if (sqlite3_exec(cache_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        log::warn(std::string("EmbeddingClient: cache schema: ") + (err ? err : "?"));
        if (err) sqlite3_free(err);
    }
}

EmbeddingClient::~EmbeddingClient() {
    if (cache_) sqlite3_close(cache_);
}

bool EmbeddingClient::cacheGet_(const std::string& key, std::vector<float>& out) {
    if (!cache_) return false;
    std::lock_guard<std::mutex> lk(cache_mu_);
    sqlite3_stmt* st = nullptr;
    const char* q = "SELECT dim, vec FROM embed_cache WHERE key = ? AND model = ?";
    if (sqlite3_prepare_v2(cache_, q, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, key.c_str(),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, cfg_.model.c_str(),  -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        int dim = sqlite3_column_int(st, 0);
        if (dim == cfg_.dim) {
            const void* blob = sqlite3_column_blob(st, 1);
            int bytes = sqlite3_column_bytes(st, 1);
            if (blob && bytes == dim * static_cast<int>(sizeof(float))) {
                out.resize(dim);
                std::memcpy(out.data(), blob, bytes);
                found = true;
            }
        }
    }
    sqlite3_finalize(st);
    return found;
}

void EmbeddingClient::cachePut_(const std::string& key, const std::vector<float>& v) {
    if (!cache_ || v.empty()) return;
    std::lock_guard<std::mutex> lk(cache_mu_);
    sqlite3_stmt* st = nullptr;
    const char* q = "INSERT OR REPLACE INTO embed_cache(key, model, dim, vec) "
                    "VALUES (?, ?, ?, ?)";
    if (sqlite3_prepare_v2(cache_, q, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text (st, 1, key.c_str(),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, cfg_.model.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 3, static_cast<int>(v.size()));
    sqlite3_bind_blob (st, 4, v.data(),
                       static_cast<int>(v.size() * sizeof(float)),
                       SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::vector<float> EmbeddingClient::embedRemote_(const std::string& text) {
    std::vector<float> out;
    if (text.empty()) return out;

    CURL* curl = curl_easy_init();
    if (!curl) return out;

    const std::string url = cfg_.ollama_url + "/api/embeddings";
    json body = { {"model", cfg_.model}, {"prompt", text} };
    std::string payload = body.dump();
    std::string response;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        long(cfg_.timeout_sec));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, long(cfg_.connect_timeout_sec));

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        log::debug(std::string("EmbeddingClient: curl ") + curl_easy_strerror(rc));
        return out;
    }

    try {
        auto j = json::parse(response);
        if (!j.contains("embedding") || !j["embedding"].is_array()) {
            log::warn("EmbeddingClient: response missing 'embedding'");
            return out;
        }
        out.reserve(j["embedding"].size());
        for (auto& v : j["embedding"]) out.push_back(v.get<float>());
        if (static_cast<int>(out.size()) != cfg_.dim) {
            log::warn("EmbeddingClient: dim mismatch " +
                      std::to_string(out.size()) + " != " + std::to_string(cfg_.dim));
            out.clear();
            return out;
        }
        l2norm(out);
        reachable_.store(true, std::memory_order_relaxed);
    } catch (const std::exception& e) {
        log::warn(std::string("EmbeddingClient: parse: ") + e.what());
        out.clear();
    }
    return out;
}

std::vector<float> EmbeddingClient::embed(const std::string& text) {
    if (text.empty()) return {};
    auto key = cacheKey_(cfg_.model, text);

    std::vector<float> v;
    if (cacheGet_(key, v)) return v;

    v = embedRemote_(text);
    if (!v.empty()) cachePut_(key, v);
    return v;
}

bool EmbeddingClient::probe() {
    // Cheap: a one-token embed. Re-uses the normal path so cache is warmed too.
    if (reachable_.load(std::memory_order_relaxed)) return true;
    auto v = embed("ping");
    return !v.empty();
}

int EmbeddingClient::cacheSize() const {
    if (!cache_) return 0;
    std::lock_guard<std::mutex> lk(cache_mu_);
    sqlite3_stmt* st = nullptr;
    int n = 0;
    if (sqlite3_prepare_v2(cache_, "SELECT COUNT(*) FROM embed_cache",
                           -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

} // namespace arise
