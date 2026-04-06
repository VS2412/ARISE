#pragma once
#include <string>
#include <deque>

struct AgentAction {
    std::string type;
    std::string param;
};

struct LLMResponse {
    std::string speech;
    AgentAction action;
    bool hasAction() const { return !action.type.empty(); }
};

struct LLMContext {
    std::string activeApp;
    std::string activeWindow;
    std::string clipboard;
    std::string memorySummary;
};

class LLM {
public:
    explicit LLM(const std::string& model = "llama3.1");
    LLMResponse think(const std::string& userText, const LLMContext& ctx = {});
    void clearHistory();

private:
    std::string model_;
    struct Message { std::string role, content; };
    std::deque<Message> history_;
    static constexpr int MAX_HISTORY = 8;

    static size_t writeCallback(void*, size_t, size_t, std::string*);
    std::string   post(const std::string& url, const std::string& body);
    LLMResponse   parse(const std::string& raw);
    std::string   buildSystem(const LLMContext& ctx);
};