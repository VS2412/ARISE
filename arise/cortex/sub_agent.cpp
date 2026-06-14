#include "cortex/sub_agent.hpp"

#include <chrono>
#include <cstring>

#include <curl/curl.h>

using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

std::size_t writeCb(void* ptr, std::size_t sz, std::size_t nm, std::string* out) {
    out->append(static_cast<char*>(ptr), sz * nm);
    return sz * nm;
}

} // namespace

std::string SubAgent::stripThinkingBlocks(std::string_view in) {
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
        if (close == std::string_view::npos) break;     // unterminated — drop tail
        i = close + std::strlen("</think>");
    }
    return out;
}

std::optional<std::string> SubAgent::firstJsonObject(std::string_view s) {
    int  depth  = 0;
    bool in_str = false;
    bool esc    = false;
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
        if (c == '{') { if (depth == 0) start = i; ++depth; }
        else if (c == '}') {
            if (depth == 0) continue;
            --depth;
            if (depth == 0 && start != std::string_view::npos) {
                return std::string(s.substr(start, i - start + 1));
            }
        }
    }
    return std::nullopt;
}

SubAgent::SubAgent(Config cfg) : cfg_(std::move(cfg)) {}

std::string SubAgent::callOllama_(const std::string& prompt,
                                  int& out_duration_ms,
                                  std::string& out_error) const {
    out_duration_ms = 0;
    out_error.clear();

    CURL* curl = curl_easy_init();
    if (!curl) { out_error = "curl_easy_init failed"; return {}; }

    const std::string url = cfg_.ollama_url + "/api/generate";

    json options = {
        {"num_gpu",     cfg_.num_gpu},
        {"temperature", cfg_.temperature},
        {"num_predict", cfg_.max_predict},
    };

    json body = {
        {"model",      cfg_.model},
        {"prompt",     prompt},
        {"stream",     false},
        {"keep_alive", cfg_.keep_alive},
        {"options",    options},
    };
    if (!cfg_.system_prompt.empty()) body["system"] = cfg_.system_prompt;
    if (cfg_.format_json)            body["format"] = "json";

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

    auto start = steady_clock::now();
    CURLcode rc = curl_easy_perform(curl);
    out_duration_ms = int(duration_cast<milliseconds>(
                              steady_clock::now() - start).count());
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        out_error = std::string("curl: ") + curl_easy_strerror(rc);
        return {};
    }

    try {
        auto j = json::parse(response);
        if (j.contains("error")) {
            out_error = std::string("ollama: ") + j["error"].get<std::string>();
            return {};
        }
        if (j.contains("response") && j["response"].is_string()) {
            return j["response"].get<std::string>();
        }
        out_error = "ollama: response missing 'response' field";
    } catch (const std::exception& e) {
        out_error = std::string("ollama: parse: ") + e.what();
    }
    return {};
}

SubAgent::Result SubAgent::run(std::string_view task) const {
    Result r;

    if (task.empty()) {
        r.error = "empty task";
        return r;
    }

    int    duration = 0;
    std::string err;
    auto raw = callOllama_(std::string(task), duration, err);
    r.duration_ms = duration;
    r.raw         = raw;

    if (raw.empty()) {
        r.error = err;
        // CURLE_OPERATION_TIMEDOUT message contains "Timeout" — flag it as
        // budget rather than network so the caller can distinguish.
        if (err.find("Timeout") != std::string::npos
            || err.find("timed out") != std::string::npos
            || (cfg_.timeout_sec > 0 && duration >= cfg_.timeout_sec * 1000 - 50)) {
            r.budget_hit = true;
        }
        return r;
    }

    r.reachable = true;
    reachable_.store(true, std::memory_order_relaxed);

    std::string body = cfg_.strip_thinking ? stripThinkingBlocks(raw)
                                            : raw;

    if (cfg_.format_json) {
        if (auto blob = firstJsonObject(body)) {
            try {
                r.json = json::parse(*blob);
                r.output = *blob;
                r.ok = true;
                return r;
            } catch (const std::exception& e) {
                r.error = std::string("json parse: ") + e.what();
                r.output = body;
                return r;
            }
        }
        r.error = "no balanced JSON object in response";
        r.output = body;
        return r;
    }

    r.output = std::move(body);
    r.ok     = !r.output.empty();
    if (!r.ok) r.error = "empty body after strip";
    return r;
}

} // namespace arise
