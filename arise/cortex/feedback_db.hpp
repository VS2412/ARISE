#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cortex/suggestion.hpp"

namespace arise {

// User reaction to a published suggestion.
enum class Decision {
    Pending,    // proposed but no response yet
    Accepted,
    Rejected,
    Ignored,    // user explicitly dismissed without action
    Timedout,   // expired without response
};

const char*               decisionToString(Decision d);
std::optional<Decision>   decisionFromString(std::string_view s);

struct SuggestionRow {
    std::int64_t           id = 0;
    Tier                   tier = Tier::Ambient;
    std::string            category;
    std::string            text;
    std::string            source_topic;
    std::string            source_payload_json;
    std::chrono::system_clock::time_point  proposed_at;
    Decision               decision = Decision::Pending;
    std::optional<std::chrono::system_clock::time_point> decided_at;
};

struct FeedbackQuery {
    std::optional<Decision>     decision;
    std::optional<Tier>         tier;
    std::string                 category;
    int                         limit = 50;
};

// SQLite-backed suggestion log under <root>/memory/feedback.db.
//
// Each suggestion the proactive engine emits gets one row with
// `decision='pending'` until the user (or a timeout sweeper later) calls
// `recordDecision`. Per-category aggregates (`consecutiveRejects`,
// `acceptance_rate`) are computed on demand — small dataset, no need to
// pre-materialise yet.
class FeedbackDb {
public:
    struct Config {
        std::string  db_path;          // <root>/memory/feedback.db
    };

    explicit FeedbackDb(Config cfg);
    ~FeedbackDb();
    FeedbackDb(const FeedbackDb&)            = delete;
    FeedbackDb& operator=(const FeedbackDb&) = delete;

    // Insert a fresh row for `s`. The Suggestion's id is updated in place
    // with the new rowid. Returns 0 on failure.
    std::int64_t recordProposed(Suggestion& s);

    // Stamp a decision on an existing row. No-ops if the id doesn't exist
    // or the row is already terminally decided.
    bool recordDecision(std::int64_t id, Decision d);

    std::optional<SuggestionRow> get(std::int64_t id) const;
    std::vector<SuggestionRow>   list(const FeedbackQuery& q) const;

    // Counters consulted by SuggestionGate.
    int consecutiveRejects(const std::string& category) const;
    int categoryCount(const std::string& category, Decision d) const;

    // Maintenance.
    int timeoutPending(std::chrono::seconds older_than);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace arise
