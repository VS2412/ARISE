#pragma once
#include <string>

struct AgentAction {
    std::string type;   // open | run | type | workspace | volume | ""
    std::string param;
};

struct LLMResponse {
    std::string speech;
    AgentAction action;
    bool hasAction() const { return !action.type.empty(); }
};

class LLM {
public:
    explicit LLM(const std::string& model = "mistral");
    LLMResponse think(const std::string& userText);
private:
    std::string model_;
    static size_t writeCallback(void* ptr, size_t size, size_t nmemb, std::string* out);
    std::string post(const std::string& url, const std::string& body);
    LLMResponse parse(const std::string& ollamaRaw);
};