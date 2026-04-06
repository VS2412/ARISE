#include "llm.hpp"
#include "logger.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>

using json = nlohmann::json;

// System prompt — personality + context only, NO JSON instructions
// Tool-calling is handled natively, not through prompting
static const std::string kPersonality = R"(You are ARIA, an AI presence embedded directly into this Arch Linux system. You have full control of this machine through your tools.

Personality: calm, sharp, occasionally dry. You live in this system. You know the user.

Rules:
- Use tools ONLY for performing actions (opening apps, changing volume, etc.)
- NEVER call a tool to answer a question — use the CURRENT SYSTEM STATE provided below instead
- If asked "what app am I using" — read Active app from state and say it
- If asked "what's in my clipboard" — read Clipboard from state and say it  
- For conversation and questions: one sentence, no markdown, no apologies
- For actions: call the appropriate tool immediately, no explanation)";

// Tool definitions — llama3.1 was trained on these, it understands them natively
static json buildTools() {
    return json::array({
        {
            {"type","function"},
            {"function",{
                {"name","open_application"},
                {"description","Launch any application on the system"},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"app",{{"type","string"},{"description","Application binary name e.g. firefox, alacritty, code, spotify"}}}}},
                    {"required",{"app"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","run_command"},
                {"description","Execute a shell command"},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"command",{{"type","string"},{"description","Shell command to run"}}}}},
                    {"required",{"command"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","type_text"},
                {"description","Type text into the currently focused window"},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"text",{{"type","string"},{"description","Text to type"}}}}},
                    {"required",{"text"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","switch_workspace"},
                {"description","Switch to a niri workspace by number"},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"number",{{"type","string"},{"description","Workspace number as string"}}}}},
                    {"required",{"number"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","close_window"},
                {"description","Close the currently focused window"},
                {"parameters",{{"type","object"},{"properties",json::object()},{"required",json::array()}}}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","control_volume"},
                {"description","Control system volume"},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"direction",{{"type","string"},{"enum",{"up","down","mute"}}}}}},
                    {"required",{"direction"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","control_brightness"},
                {"description","Control screen brightness"},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"direction",{{"type","string"},{"enum",{"up","down"}}}}}},
                    {"required",{"direction"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","control_media"},
                {"description","Control media playback"},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"action",{{"type","string"},{"enum",{"play","pause","next","prev","stop"}}}}}},
                    {"required",{"action"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","take_screenshot"},
                {"description","Take a screenshot of the current screen"},
                {"parameters",{{"type","object"},{"properties",json::object()},{"required",json::array()}}}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","open_url"},
                {"description","Open a URL in the browser"},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"url",{{"type","string"},{"description","Full URL including https://"}}}}},
                    {"required",{"url"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","send_notification"},
                {"description","Send a desktop notification"},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"message",{{"type","string"},{"description","Notification text"}}}}},
                    {"required",{"message"}}
                }}
            }}
        }
    });
}

// Map tool name + arguments → AgentAction (executor understands these)
static AgentAction toolToAction(const std::string& name, const json& args) {
    if (name == "open_application")  return {"open",       args.value("app",       "")};
    if (name == "run_command")       return {"run",        args.value("command",   "")};
    if (name == "type_text")         return {"type",       args.value("text",      "")};
    if (name == "switch_workspace")  return {"workspace",  args.value("number",    "")};
    if (name == "close_window")      return {"close",      ""};
    if (name == "control_volume")    return {"volume",     args.value("direction", "")};
    if (name == "control_brightness")return {"brightness", args.value("direction", "")};
    if (name == "control_media")     return {"media",      args.value("action",    "")};
    if (name == "take_screenshot")   return {"screenshot", ""};
    if (name == "open_url")          return {"url",        args.value("url",       "")};
    if (name == "send_notification") return {"notify",     args.value("message",   "")};
    return {"", ""};
}

size_t LLM::writeCallback(void* ptr, size_t sz, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

LLM::LLM(const std::string& model) : model_(model) {}

void LLM::clearHistory() { history_.clear(); }

std::string LLM::buildSystem(const LLMContext& ctx) {
    std::ostringstream s;
    s << kPersonality;
    s << "\n\nCURRENT SYSTEM STATE:";
    if (!ctx.activeApp.empty())    s << "\nActive app: "    << ctx.activeApp;
    if (!ctx.activeWindow.empty()) s << "\nWindow title: "  << ctx.activeWindow;
    if (!ctx.clipboard.empty())    s << "\nClipboard: "     << ctx.clipboard;
    if (!ctx.memorySummary.empty())s << "\n"                << ctx.memorySummary;
    return s.str();
}

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

        // catch Ollama error responses
        if (outer.contains("error")) {
            Logger::error("LLM: Ollama error: " + outer["error"].get<std::string>());
            result.speech = "Model error.";
            return result;
        }

        if (!outer.contains("message")) {
            Logger::error("LLM: no message field. Raw: " + raw.substr(0, 300));
            result.speech = "No response.";
            return result;
        }

        auto& msg = outer["message"];

        // TOOL CALL path
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()
            && !msg["tool_calls"].empty()) {

            auto& call = msg["tool_calls"][0]["function"];
            std::string name = call["name"].get<std::string>();

            json args;
            auto& rawArgs = call["arguments"];
            if (rawArgs.is_string()) {
                try { args = json::parse(rawArgs.get<std::string>()); }
                catch (...) { args = json::object(); }
            } else {
                args = rawArgs;
            }

            result.action = toolToAction(name, args);
            Logger::info("LLM: tool call → " + name + " | " + args.dump());
            return result;
        }

        // CONVERSATION path
        std::string text;
        if (msg.contains("content") && !msg["content"].is_null())
            text = msg["content"].get<std::string>();

        auto s = text.find_first_not_of(" \t\n\r");
        auto e = text.find_last_not_of(" \t\n\r");
        if (s == std::string::npos) { result.speech = "Nothing to say."; return result; }
        result.speech = text.substr(s, e - s + 1);

    } catch (const std::exception& ex) {
        Logger::error("LLM: parse error: " + std::string(ex.what()));
        Logger::error("LLM: raw was: " + raw.substr(0, 400));
        result.speech = "Something went wrong.";
    }

    return result;
}

LLMResponse LLM::think(const std::string& userText, const LLMContext& ctx) {
    Logger::info("LLM: prompt → " + userText);

    // Build message history
    json messages = json::array();
    for (const auto& m : history_)
        messages.push_back({{"role", m.role}, {"content", m.content}});
    messages.push_back({{"role", "user"}, {"content", userText}});

    json body;
    body["model"]    = model_;
    body["stream"]   = false;
    body["system"]   = buildSystem(ctx);
    body["messages"] = messages;
    body["tools"]    = buildTools();  // native tool-calling

    auto t0  = std::chrono::steady_clock::now();
    auto raw = post("http://localhost:11434/api/chat", body.dump());
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    Logger::info("LLM: done in " + std::to_string(ms) + " ms");

    auto result = parse(raw);

    // Only store conversational turns — not tool calls
    if (!result.hasAction() && !result.speech.empty()) {
        history_.push_back({"user",      userText});
        history_.push_back({"assistant", result.speech});
        while (static_cast<int>(history_.size()) > MAX_HISTORY)
            history_.pop_front();
    }

    return result;
}