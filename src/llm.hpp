#pragma once
#include <string>
#include <deque>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

struct AgentAction {
    std::string type;
    nlohmann::json args = nlohmann::json::object();
    bool empty() const { return type.empty(); }
};

struct LLMResponse {
    std::string speech;
    std::vector<AgentAction> actions;
    bool done = false;
    bool hasAction() const { return !actions.empty(); }
};

struct LLMContext {
    std::string activeApp;
    std::string activeWindow;
    std::string clipboard;
    std::string memorySummary;
    std::string screenText;
    std::string tone;          // detected user mood: "", "frustrated", "urgent", "casual", "curious"
    std::string timeOfDay;     // "morning" | "afternoon" | "evening" | "night"
    std::string dateLabel;     // e.g. "Thursday, April 15"
};

using StreamCallback = std::function<void(const std::string& delta)>;

class LLM {
public:
    explicit LLM(const std::string& model = "llama3.1");

    // Batch API (backwards compatible — calls streaming with null callback)
    LLMResponse think(const std::string& userText, const LLMContext& ctx = {});
    LLMResponse react(const std::string& observation, const LLMContext& ctx);

    // Streaming API — onDelta fires for each text token as it arrives
    LLMResponse thinkStreaming(const std::string& userText, const LLMContext& ctx,
                               StreamCallback onDelta);
    LLMResponse reactStreaming(const std::string& observation, const LLMContext& ctx,
                               StreamCallback onDelta);

    // One-shot summarization — no history, no tools. Returns plain text.
    std::string summarize(const std::string& conversationText);

    void clearHistory();

private:
    std::string model_;
    struct Message { std::string role, content; };
    std::deque<Message> history_;
    static constexpr int MAX_HISTORY = 16;

    static size_t writeCallback(void*, size_t, size_t, std::string*);
    std::string   post(const std::string& url, const std::string& body);
    LLMResponse   parse(const std::string& raw);
    std::string   buildSystem(const LLMContext& ctx);

    // Streaming internals
    struct StreamState {
        std::string lineBuf;       // partial line accumulator
        std::string fullContent;   // all text deltas concatenated
        StreamCallback onDelta;
        bool inThink = false;      // inside <think> block
        std::string thinkBuf;      // partial tag accumulator
        nlohmann::json finalMsg;   // the done:true message for tool calls
        bool hasFinal = false;
    };
    static size_t streamWriteCallback(void* ptr, size_t sz, size_t nmemb, StreamState* state);
    static void   processStreamLine(const std::string& line, StreamState& state);
    static std::string stripThinkTags(const std::string& text);

    LLMResponse postStreaming(const std::string& url, const std::string& body,
                              StreamCallback onDelta);
    LLMResponse chatStreaming(const nlohmann::json& messages, const LLMContext& ctx,
                              StreamCallback onDelta);
    void updateHistory(const std::string& userText, const LLMResponse& result,
                       const std::string& userRole = "user");
};
