#pragma once
#include <string>
#include "llm.hpp"

class Executor {
public:
    Executor() = default;
    std::string execute(const AgentAction& action);
    std::string shellCapture(const std::string& cmd);

    // Safety-guarded variant — runs the same checkSafety deny list that
    // Executor::execute("run") uses. Returns the refusal message if denied,
    // otherwise captures command output. ALL LLM-driven command execution
    // (including the ReAct observation path) must go through this, not
    // shellCapture() directly.
    std::string safeShellCapture(const std::string& cmd);
private:
    void shell(const std::string& cmd);
};