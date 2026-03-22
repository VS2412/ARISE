#pragma once
#include <string>
#include "llm.hpp"

class Executor {
public:
    Executor() = default;
    std::string execute(const AgentAction& action);
private:
    void shell(const std::string& cmd);
};