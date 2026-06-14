#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace arise {

class SubAgent;

// Pre-flight safety review of arbitrary content (a shell snippet, a diff, a
// generated script, a tool argument). Phase 4 commit 2 ships the deterministic
// regex denylist; the LLM-as-judge path is opt-in.
//
// The deny list is hard-coded patterns that have a very high cost when they
// slip through (`rm -rf /`, fork bombs, `curl ... | sh` chains, world-write
// chmod) — these always reject regardless of the LLM's opinion. The LLM is
// only consulted when the regex layer is silent and the caller asked for
// `require_llm_for_approval` — so an empty Ollama deployment can never
// auto-approve dangerous content, only auto-reject it.
class Critic {
public:
    struct Config {
        SubAgent*                llm = nullptr;          // optional
        std::vector<std::string> additional_denylist;     // user-supplied regex
        // When true, Critic refuses to approve any content unless the LLM has
        // explicitly endorsed it. When false (default), passing the regex
        // gauntlet alone is enough — useful for offline tests + when no LLM
        // is configured.
        bool                     require_llm_for_approval = false;
        // Soft cap on content length passed to the LLM (chars); longer
        // content is truncated tail-first before the prompt.
        int                      llm_max_chars            = 6000;
    };

    struct Review {
        bool                     approved = false;
        std::string              verdict;     // human-readable
        std::vector<std::string> matches;     // denylist patterns that fired
        bool                     from_llm = false;
        std::string              llm_raw;     // diagnostic only
    };

    explicit Critic(Config cfg);

    // `kind` is a free-form label ("shell", "diff", "script") used in the
    // prompt and verdict so the LLM knows what it's looking at.
    Review reviewContent(const std::string& content,
                         std::string_view kind) const;

    // Just the regex layer — used by tests and by callers that want to
    // pre-screen before deciding whether to consult the LLM.
    static std::vector<std::string> matchDenylist(const std::string& content,
                                                  const std::vector<std::string>&
                                                      extra_patterns = {});

    // Built-in denylist (lowercased substrings + a small set of regex-like
    // patterns we walk manually to avoid pulling in a regex dependency).
    static const std::vector<std::string>& builtinDenylist();

    const Config& config() const { return cfg_; }

private:
    Config cfg_;

    Review askLlm_(const std::string& content, std::string_view kind) const;
};

} // namespace arise
