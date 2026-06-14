#include "cortex/salience.hpp"
#include "util/log.hpp"

#include <cmath>
#include <cstring>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace arise {

namespace {

std::size_t writeCb(void* ptr, std::size_t sz, std::size_t nm, std::string* out) {
    out->append(static_cast<char*>(ptr), sz * nm);
    return sz * nm;
}

// Remove any <think>...</think> blocks (qwen3 reasoning prefix). Greedy + nested-safe.
std::string stripThinkingBlocks(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        auto open = in.find("<think>", i);
        if (open == std::string_view::npos) {
            out.append(in.substr(i));
            break;
        }
        out.append(in.substr(i, open - i));
        auto close = in.find("</think>", open);
        if (close == std::string_view::npos) {
            // unterminated — drop the rest
            break;
        }
        i = close + std::strlen("</think>");
    }
    return out;
}

// Find the first balanced top-level JSON object substring. Naive but fine
// for the tiny payloads we expect (one-line {"salience":...}).
std::optional<std::string> firstJsonObject(std::string_view s) {
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    std::size_t start = std::string_view::npos;

    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"')  { in_str = false; continue; }
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (c == '}') {
            if (depth == 0) continue;
            --depth;
            if (depth == 0 && start != std::string_view::npos) {
                return std::string(s.substr(start, i - start + 1));
            }
        }
    }
    return std::nullopt;
}

double clamp01(double x) {
    if (std::isnan(x)) return 0.0;
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

} // namespace

SalienceScorer::SalienceScorer(Config cfg) : cfg_(std::move(cfg)) {}

std::optional<SalienceScore> SalienceScorer::parse(std::string_view text,
                                                   bool strip_thinking) {
    std::string cleaned = strip_thinking
        ? stripThinkingBlocks(text)
        : std::string(text);

    auto blob = firstJsonObject(cleaned);
    if (!blob) return std::nullopt;

    try {
        auto j = json::parse(*blob);
        if (!j.contains("salience")) return std::nullopt;
        SalienceScore out;
        // Accept number, numeric string, or stringified percent.
        if (j["salience"].is_number()) {
            out.salience = clamp01(j["salience"].get<double>());
        } else if (j["salience"].is_string()) {
            try {
                out.salience = clamp01(std::stod(j["salience"].get<std::string>()));
            } catch (...) { return std::nullopt; }
        } else {
            return std::nullopt;
        }
        if (j.contains("reason") && j["reason"].is_string()) {
            out.reason = j["reason"].get<std::string>();
        }
        out.from_llm = true;
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string SalienceScorer::callOllama_(const std::string& prompt) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    const std::string url = cfg_.ollama_url + "/api/generate";

    json body = {
        {"model",      cfg_.model},
        {"prompt",     prompt},
        {"stream",     false},
        {"format",     "json"},
        {"keep_alive", cfg_.keep_alive},
        {"options",    {
            {"num_gpu",     cfg_.num_gpu},
            {"temperature", 0.0},
            {"num_predict", 80},
        }},
    };
    std::string payload  = body.dump();
    std::string response;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  long(payload.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        long(cfg_.timeout_sec));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, long(cfg_.connect_timeout_sec));

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        log::debug(std::string("SalienceScorer: curl ") + curl_easy_strerror(rc));
        return {};
    }

    try {
        auto j = json::parse(response);
        if (j.contains("error")) {
            log::warn("SalienceScorer: ollama error: " +
                      j["error"].get<std::string>());
            return {};
        }
        if (j.contains("response") && j["response"].is_string()) {
            return j["response"].get<std::string>();
        }
    } catch (const std::exception& e) {
        log::warn(std::string("SalienceScorer: parse: ") + e.what());
    }
    return {};
}

SalienceScore SalienceScorer::score(const std::string& kind,
                                    const std::string& summary) {
    SalienceScore fallback;
    fallback.salience = cfg_.default_score;
    fallback.from_llm = false;

    if (summary.empty()) return fallback;

    // Build a tight, JSON-only prompt. Rubric is hand-tuned for screen + audio
    // observations; "0 = noise (idle desktop, blank screen, ambient room)",
    // "1 = pivotal (error message, identity moment, decision)".
    std::string prompt =
        "You are ARISE's salience filter. Decide whether this observation is "
        "worth keeping in long-term memory. Reply with JSON only — no prose, "
        "no markdown — exactly: {\"salience\": <float 0..1>, \"reason\": "
        "\"<short phrase>\"}.\n"
        "Rubric:\n"
        "  0.0  pure noise (blank screen, ambient silence)\n"
        "  0.2  routine background (lock screen, music playing)\n"
        "  0.4  mildly informative (app switch, normal browsing)\n"
        "  0.6  notable (error visible, decision moment, named person)\n"
        "  0.8  pivotal (commit, ship, breakthrough, important conversation)\n"
        "  1.0  critical (security alert, emergency, identity-defining)\n"
        "Observation kind: " + kind + "\n"
        "Observation: " + summary + "\n"
        "JSON:";

    auto raw = callOllama_(prompt);
    if (raw.empty()) return fallback;

    auto parsed = parse(raw, cfg_.strip_thinking);
    if (!parsed) {
        log::debug("SalienceScorer: unparseable response: " + raw.substr(0, 120));
        return fallback;
    }
    reachable_.store(true, std::memory_order_relaxed);
    return *parsed;
}

} // namespace arise
