// test_agent_action.cpp
// -----------------------------------------------------------------------------
// Boundary / edge-case tests for AgentAction and LLMResponse.
// This suite validates extreme or unusual states that the LLM might return
// unexpectedly, ensuring the OS doesn't crash on malformed data.
// -----------------------------------------------------------------------------

#include <gtest/gtest.h>
#include "llm.hpp"

// ── Edge Cases ────────────────────────────────────────────────────────────────

// Verifies that a massive parameter string (e.g. if the LLM hallucinates 
// an infinitely long shell command) does not crash the data structure.
TEST(AgentActionEdge, LongParamDoesNotCrash) {
    AgentAction a;
    a.type  = "run";
    // Create a 10,000 character string of 'x's
    a.param = std::string(10000, 'x'); 
    EXPECT_EQ(a.param.size(), 10000u);
}

// Verifies that multi-byte Unicode characters (emojis, non-latin scripts) 
// are preserved safely by the string implementation.
TEST(AgentActionEdge, UnicodeParamPreserved) {
    AgentAction a;
    a.type  = "type";
    a.param = "नमस्ते ARISE 🚀"; // Hindi greeting + rocket emoji
    EXPECT_FALSE(a.param.empty());
}

// Verifies that an explicitly empty speech string is handled gracefully.
TEST(LLMResponseEdge, EmptySpeechIsValid) {
    LLMResponse r;
    r.speech = "";
    EXPECT_TRUE(r.speech.empty());
    EXPECT_FALSE(r.hasAction());
}

// Some actions (like 'mute' or 'lock_screen') might not require parameters.
// This verifies that an action is still valid even if param is empty.
TEST(LLMResponseEdge, ActionWithEmptyParamStillHasAction) {
    LLMResponse r;
    r.action.type  = "mute";
    r.action.param = "";
    EXPECT_TRUE(r.hasAction());
}

// Verifies state isolation — if we clear the action type, hasAction() 
// correctly reflects that the response no longer contains an action.
TEST(LLMResponseEdge, ResetActionTypeRemovesAction) {
    LLMResponse r;
    r.action.type  = "open";
    r.action.param = "firefox";
    EXPECT_TRUE(r.hasAction());

    r.action.type = "";  // clear it
    EXPECT_FALSE(r.hasAction());
}
