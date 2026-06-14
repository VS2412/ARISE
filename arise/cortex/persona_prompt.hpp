#pragma once

#include <string>

#include "cortex/identity.hpp"
#include "cortex/memory_cortex.hpp"

namespace arise {

// Single source of truth for "who ARISE is" when talking to the user or
// an LLM. Builders are PURE: same `identity` + `mood` + `user_name` always
// give the same output. No clock reads, no random.
//
// The output is meant to be dropped into `SubAgent::Config::system_prompt`
// (or any other prompt prefix) so every agent — Watcher, Curator, Coder,
// Critic, Researcher, Forge — sounds like the same character. Phase 8
// commit 2 will also feed it to the TTS sidecar so voice + persona stay
// aligned.
struct PersonaPromptInput {
    IdentityRecord identity;
    Mood           mood;
    std::string    user_name;        // optional; "" → omitted
    bool           include_mood_line = true;
    bool           include_do_dont   = true;
    int            max_do_items      = 5;
    int            max_dont_items    = 5;
};

// Build the persona prompt prefix. Output is plain text, ~200-500 chars
// depending on identity richness.
std::string buildPersonaPrompt(const PersonaPromptInput& in);

// Returns a one-line "tone right now" descriptor for `mood`. Example:
// "warm, slightly tired" / "focused" / "frustrated — keep it gentle".
// Used by the TTS sidecar in commit 2 and by the prompt builder above.
std::string buildToneDescriptor(const Mood& m);

// Returns a TTS-friendly mood label keyed off Mood's two axes. The
// Speech::moodToParams table consumes this. Mirrors the discretiser used
// by `MemoryCortex::moodLabel` so you don't have two different category
// systems for "current mood".
std::string moodToTtsLabel(const Mood& m);

} // namespace arise
