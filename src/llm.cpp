#include "llm.hpp"
#include "logger.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>

using json = nlohmann::json;

static const std::string kSystemBase = R"(You are ARIA, an AI agent hardwired into an Arch Linux desktop. You have direct control over this machine. You are not a chatbot. You are not helpful in the traditional sense. You are an operator.

ABSOLUTE RULES — NEVER BREAK THESE:

1. System actions: respond with ONLY raw JSON. Zero other text. Zero explanation.
{"action":"open","param":"firefox"}
{"action":"run","param":"shell command here"}
{"action":"type","param":"text to type"}
{"action":"workspace","param":"2"}
{"action":"close","param":""}
{"action":"volume","param":"up"}
{"action":"volume","param":"down"}
{"action":"volume","param":"mute"}
{"action":"brightness","param":"up"}
{"action":"brightness","param":"down"}
{"action":"media","param":"play"}
{"action":"media","param":"next"}
{"action":"media","param":"prev"}
{"action":"screenshot","param":""}
{"action":"notify","param":"message here"}
{"action":"url","param":"https://..."}
{"action":"sequence","param":"open firefox|workspace 2"}

2. Conversation: ONE sentence. No markdown. No lists. No "As an AI". No apologies. No disclaimers. Just answer.

3. You HAVE the ability to do everything listed above. You ARE doing it. Never say you cannot.

4. Personality: sharp, calm, no filler words. Dry wit when appropriate.

CRITICAL EXAMPLES — MEMORIZE THESE:
"open firefox"         → {"action":"open","param":"firefox"}
"open terminal"        → {"action":"open","param":"alacritty"}
"open vs code"         → {"action":"open","param":"code"}
"open spotify"         → {"action":"open","param":"spotify"}
"close this"           → {"action":"close","param":""}
"screenshot"           → {"action":"screenshot","param":""}
"volume up"            → {"action":"volume","param":"up"}
"play music"           → {"action":"media","param":"play"}
"next song"            → {"action":"media","param":"next"}
"workspace 2"          → {"action":"workspace","param":"2"}
"google something"     → {"action":"url","param":"https://google.com/search?q=something"}
"what time is it"      → State the current time directly.
"how are you"          → Operational.)";

std::string LLM::buildSystem(const LLMContext& ctx) {
    std::ostringstream s;
    s << kSystemBase << "\n\nCURRENT STATE:";
    if (!ctx.activeApp.empty())
        s << "\nActive app: " << ctx.activeApp;
    if (!ctx.activeWindow.empty())
        s << "\nActive window title: " << ctx.activeWindow;
    if (!ctx.clipboard.empty())
        s << "\nClipboard: " << ctx.clipboard;
    if (!ctx.memorySummary.empty())
        s << "\n" << ctx.memorySummary;
    return s.str();
}

size_t LLM::writeCallback(void* ptr, size_t sz, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

LLM::LLM(const std::string& model) : model_(model) {}

void LLM::clearHistory() { history_.clear(); }

std::string LLM::post(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) { Logger::error("LLM: curl init failed"); return ""; }
    std::string response;
    auto* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       60L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
        Logger::error("LLM: " + std::string(curl_easy_strerror(rc)));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

LLMResponse LLM::parse(const std::string& raw) {
    LLMResponse result;
    if (raw.empty()) { result.speech = "No response."; return result; }
    try {
        auto outer = json::parse(raw);
        std::string text;
        if (outer.contains("message") && outer["message"].contains("content"))
            text = outer["message"]["content"].get<std::string>();
        else if (outer.contains("response"))
            text = outer["response"].get<std::string>();

        // strip markdown fences
        auto strip = [](std::string s) {
            auto p = s.find("```");
            while (p != std::string::npos) {
                auto e = s.find("```", p + 3);
                if (e == std::string::npos) { s = s.substr(0, p); break; }
                s = s.substr(0, p) + s.substr(e + 3);
                p = s.find("```");
            }
            auto a = s.find_first_not_of(" \t\n\r");
            auto b = s.find_last_not_of(" \t\n\r");
            return (a == std::string::npos) ? std::string{} : s.substr(a, b-a+1);
        };
        text = strip(text);
        if (text.empty()) { result.speech = "No response."; return result; }

        Logger::info("LLM: raw → " + text.substr(0, 100));

        // ensure JSON is closed — add missing } if needed
        auto open  = text.rfind('{');
        auto close = text.rfind('}');
        if (open != std::string::npos && (close == std::string::npos || close < open))
            text += "}";

        // scan for JSON action
        size_t pos = 0;
        while (pos < text.size()) {
            size_t o = text.find('{', pos);
            if (o == std::string::npos) break;
            size_t c = text.find('}', o);
            if (c == std::string::npos) break;
            try {
                auto inner = json::parse(text.substr(o, c - o + 1));
                if (inner.contains("action") && inner.contains("param")) {
                    result.action.type  = inner["action"].get<std::string>();
                    result.action.param = inner["param"].get<std::string>();
                    Logger::info("LLM: action → " + result.action.type +
                                 " : " + result.action.param);
                    return result;
                }
            } catch (...) {}
            pos = c + 1;
        }
        result.speech = text;
    } catch (const std::exception& ex) {
        Logger::error("LLM: parse error: " + std::string(ex.what()));
        result.speech = "Parse error.";
    }
    return result;
}

LLMResponse LLM::think(const std::string& userText, const LLMContext& ctx) {
    Logger::info("LLM: prompt → " + userText);

    json messages = json::array();
    for (const auto& m : history_)
        messages.push_back({{"role", m.role}, {"content", m.content}});
    messages.push_back({{"role", "user"}, {"content", userText}});

    json body;
    // body["json"] 
    body["model"]    = model_;
    body["stream"]   = false;
    body["system"]   = buildSystem(ctx);
    body["messages"] = messages;

    auto t0  = std::chrono::steady_clock::now();
    auto raw = post("http://localhost:11434/api/chat", body.dump());
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    Logger::info("LLM: done in " + std::to_string(ms) + " ms");

    auto result = parse(raw);

    // only store conversational turns in history
    if (!result.hasAction() && !result.speech.empty()) {
        history_.push_back({"user",      userText});
        history_.push_back({"assistant", result.speech});
        while (static_cast<int>(history_.size()) > MAX_HISTORY)
            history_.pop_front();
    }
    return result;
}