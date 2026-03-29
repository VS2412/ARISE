#pragma once
#include <string>
#include "llm.hpp"

// Returns a filled AgentAction if text matches a known command pattern.
// Returns empty action if it needs LLM reasoning.
AgentAction classifyIntent(const std::string& text);