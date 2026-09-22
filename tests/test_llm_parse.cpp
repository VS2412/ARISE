// test_llm_parse.cpp
// -----------------------------------------------------------------------------
// Unit tests for the LLMResponse and AgentAction structures.
// These tests validate the underlying data models and the `hasAction()` 
// helper method. They are designed to run purely on logic without requiring 
// a live connection to the local Ollama LLM server.
// -----------------------------------------------------------------------------

#include <gtest/gtest.h>
#include <vector>   // explicitly included — do not rely on transitive includes
#include "llm.hpp"

// ── AgentAction Tests ─────────────────────────────────────────────────────────

// Verifies that a default-initialized AgentAction contains no garbage data.
TEST(AgentActionTest, EmptyTypeMeansNoAction) {
    AgentAction a;
    EXPECT_TRUE(a.type.empty());
    EXPECT_TRUE(a.param.empty());
}

// Verifies that assigning standard values correctly populates the struct.
TEST(AgentActionTest, PopulatedTypeIsValid) {
    AgentAction a;
    a.type  = "open";
    a.param = "firefox";
    EXPECT_EQ(a.type,  "open");
    EXPECT_EQ(a.param, "firefox");
}

// ── LLMResponse Tests ─────────────────────────────────────────────────────────

// Verifies that a fresh response defaults to having no executable action.
TEST(LLMResponseTest, DefaultHasNoAction) {
    LLMResponse r;
    EXPECT_FALSE(r.hasAction());
}

// Verifies that setting the action type properly triggers the hasAction flag.
TEST(LLMResponseTest, HasActionReturnsTrueWhenTypeSet) {
    LLMResponse r;
    r.action.type  = "run";
    r.action.param = "ls -la";
    EXPECT_TRUE(r.hasAction());
}

// Verifies that setting only the speech text does NOT trigger an action execution.
TEST(LLMResponseTest, SpeechFieldIsIndependentOfAction) {
    LLMResponse r;
    r.speech = "Hello, here is your result.";
    EXPECT_FALSE(r.hasAction());
    EXPECT_EQ(r.speech, "Hello, here is your result.");
}

// Verifies that an LLM can return both speech AND an action simultaneously 
// (e.g. "Opening firefox now" + the actual JSON action to open it).
TEST(LLMResponseTest, ActionAndSpeechCanCoexist) {
    LLMResponse r;
    r.speech       = "Opening the browser for you.";
    r.action.type  = "open";
    r.action.param = "chromium";
    EXPECT_TRUE(r.hasAction());
    EXPECT_FALSE(r.speech.empty());
}

// ── Known action-type vocabulary ──────────────────────────────────────────────

// Validates that the core expected actions (defined in the system prompt)
// can be represented by the struct.
TEST(AgentActionTest, KnownActionTypes) {
    const std::vector<std::string> validTypes = {
        "open", "run", "type", "workspace", "volume"
    };
    for (const auto& t : validTypes) {
        AgentAction a;
        a.type = t;
        EXPECT_FALSE(a.type.empty()) << "type should not be empty: " << t;
    }
}
