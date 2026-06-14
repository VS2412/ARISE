#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace arise {

// Pre-flight scrubber. Phase 9 commit 1 ships the deterministic regex /
// Luhn layer; the LLM-judged "is this snippet sensitive?" follow-up arrives
// with the daily learner in commit 2.
//
// Goal: every (input, response, outcome) tuple that goes into a future
// training set passes through here. The filter never *has* to redact —
// when a Trace contains anything detectable, the curator just drops the
// trace entirely. Redaction is offered for callers that want to keep the
// surrounding context (e.g. CLI inspection) without leaking the secret.
class PrivacyFilter {
public:
    enum class Reason {
        CreditCard,         // 13-19 digit run that Luhn-checks
        ApiKey,             // sk-... / ghp_... / pk_live_... / xoxb-... etc.
        BearerToken,        // "Bearer <thing>" with a long token
        SshPath,            // path containing /.ssh/
        PasswordContext,    // "password=" / "passwd:" / "secret:" with content
        AwsAccessKey,       // AKIA[0-9A-Z]{16}
        PrivateKeyHeader,   // "-----BEGIN ... PRIVATE KEY-----"
    };

    static const char* reasonToString(Reason r);

    struct Hit {
        Reason       reason;
        std::size_t  start;     // byte offset into the input
        std::size_t  length;
        std::string  preview;   // ≤ 32 chars of the matched text (clipped)
    };

    struct Result {
        bool                     passed = true;     // false ⇒ at least one hit
        std::vector<Hit>         hits;
        std::string              redacted_text;     // input with hits replaced
    };

    struct Config {
        // Replacement to substitute for every redacted match. Keep it short.
        std::string replacement = "[REDACTED]";
        // Minimum length for things that look like keys / tokens.
        int         min_token_len = 16;
        // Skip sufficiently small Luhn runs to avoid false-positives on
        // years, IDs, etc. 13 is the smallest real card length.
        int         min_card_len = 13;
        int         max_card_len = 19;
    };

    explicit PrivacyFilter(Config cfg);

    Result scan(std::string_view text) const;

    // Static utilities exposed for tests.
    static bool        luhnCheck(std::string_view digits_only);
    static std::string preview(std::string_view text,
                               std::size_t start, std::size_t len,
                               std::size_t cap = 32);

    const Config& config() const { return cfg_; }

private:
    Config cfg_;

    void scanCreditCards_   (std::string_view text, Result& r) const;
    void scanApiKeys_       (std::string_view text, Result& r) const;
    void scanBearerTokens_  (std::string_view text, Result& r) const;
    void scanSshPaths_      (std::string_view text, Result& r) const;
    void scanPasswordCtx_   (std::string_view text, Result& r) const;
    void scanAwsKeys_       (std::string_view text, Result& r) const;
    void scanPrivateKeys_   (std::string_view text, Result& r) const;
};

} // namespace arise
