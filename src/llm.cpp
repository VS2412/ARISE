#include "llm.hpp"
#include "logger.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

static const std::string kSystem = R"(You are ARIA, a voice-controlled AI desktop agent on Arch Linux with niri window manager. Your responses are spoken aloud, so:
- Never use markdown, bullet points, or formatting
- Keep all responses under 2 sentences
- Speak naturally and concisely

When the user asks you to perform a system action, respond ONLY with a single JSON object and nothing else:
{"action":"open","param":"app-name"}
{"action":"run","param":"shell command"}
{"action":"type","param":"text to type"}
{"action":"workspace","param":"1"}
{"action":"volume","param":"up"}
{"action":"volume","param":"down"}
{"action":"volume","param":"mute"}

For everything else respond conversationally in 1-2 sentences. Never mix JSON with plain text.)";

size_t LLM::writeCallback(void* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

LLM::LLM(const std::string& model) : model_(model) {}

std::string LLM::post(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) { Logger::error("LLM: curl init failed"); return ""; }

    std::string response;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       30L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
        Logger::error("LLM: " + std::string(curl_easy_strerror(rc)));

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

LLMResponse LLM::parse(const std::string& raw) {
    LLMResponse result;
    if (raw.empty()) {
        result.speech = "I could not reach the language model.";
        return result;
    }

    try {
        auto outer = json::parse(raw);

        std::string text;
        if (outer.contains("message") && outer["message"].contains("content"))
            text = outer["message"]["content"].get<std::string>();
        else if (outer.contains("response"))
            text = outer["response"].get<std::string>();

        // trim
        auto s = text.find_first_not_of(" \t\n\r");
        auto e = text.find_last_not_of(" \t\n\r");
        if (s == std::string::npos) { result.speech = "No response."; return result; }
        text = text.substr(s, e - s + 1);

        Logger::info("LLM: raw text → " + text.substr(0, 120));

        // scan entire response for a JSON action object — handles mixed prose+JSON
        size_t pos = 0;
        while (pos < text.size()) {
            size_t open = text.find('{', pos);
            if (open == std::string::npos) break;
            size_t close = text.find('}', open);
            if (close == std::string::npos) break;

            std::string candidate = text.substr(open, close - open + 1);
            try {
                auto inner = json::parse(candidate);
                if (inner.contains("action") && inner.contains("param")) {
                    result.action.type  = inner["action"].get<std::string>();
                    result.action.param = inner["param"].get<std::string>();
                    Logger::info("LLM: action → " + result.action.type +
                                 " : " + result.action.param);
                    return result; // action found, no speech needed
                }
            } catch (...) {}

            pos = close + 1;
        }

        // no action found — treat as speech
        result.speech = text;

    } catch (const std::exception& ex) {
        Logger::error("LLM: parse error: " + std::string(ex.what()));
        result.speech = "I had trouble processing that.";
    }
    return result;
}

LLMResponse LLM::think(const std::string& userText) {
    Logger::info("LLM: prompt → " + userText);

    json body;
    body["model"]  = model_;
    body["stream"] = false;
    body["messages"] = json::array({
        {{"role", "system"}, {"content", kSystem}},
        {{"role", "user"},   {"content", userText}}
    });

    auto t0  = std::chrono::steady_clock::now();
    auto raw = post("http://localhost:11434/api/chat", body.dump());
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    Logger::info("LLM: done in " + std::to_string(ms) + " ms");

    return parse(raw);
}