#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arise {

class MemoryCortex;

using GoalTimestamp = std::chrono::system_clock::time_point;

// Where in its life a goal currently sits. Lifecycle (commit 1):
//
//   Proposed → Accepted → InProgress → Done
//        ↘            ↘        ↘       ↘
//        Rejected   Cancelled  Blocked → InProgress (resumed)
//                              Cancelled
//                              Done
//
// Done / Rejected / Cancelled are terminal — `setStatus` will refuse any
// further transition (caller can re-propose a fresh row instead).
enum class GoalStatus {
    Proposed,
    Accepted,
    InProgress,
    Blocked,
    Done,
    Rejected,
    Cancelled,
};

const char*                 toString(GoalStatus s);
std::optional<GoalStatus>   goalStatusFromString(std::string_view s);

// True iff `next` is a permitted transition from `from`. Same-status writes
// are allowed (idempotent updates) but terminal states are sealed.
bool isValidTransition(GoalStatus from, GoalStatus to);

// "What ARISE has decided is worth working on, possibly across reboots."
//
// `plan_json` is owned by whatever planner produces it — GoalStore treats it
// as opaque text. `tags` are free-form labels (e.g. "cmake", "refactor")
// for cheap pre-LLM filtering and grouping.
struct Goal {
    std::int64_t                 id          = 0;
    std::string                  summary;
    GoalStatus                   status      = GoalStatus::Proposed;
    int                          priority    = 50;        // 0..100, 50 = neutral
    std::optional<GoalTimestamp> deadline_at;
    std::optional<std::int64_t>  parent_id;
    GoalTimestamp                created_at;              // auto-set on propose
    GoalTimestamp                last_progress_at;        // auto-bumped on changes
    std::string                  blocked_reason;          // empty unless Blocked
    std::string                  plan_json;               // free-form
    std::vector<std::string>     tags;
};

struct GoalQuery {
    std::optional<GoalStatus>   status;
    std::optional<std::int64_t> parent_id;
    std::string                 text;          // FTS5 search; empty = no filter
    std::string                 tag;           // single tag match; empty = ignore
    int                         limit          = 50;
    bool                        order_by_priority = false;  // false = recency
};

// SQLite-backed durable goal store with FTS5 mirror over summary +
// blocked_reason. Thread-safe — every public method takes the internal
// mutex. The store optionally writes an EpisodicEvent into a MemoryCortex
// on every meaningful state change so future recall ("what was I going to
// do about cmake?") finds the goal milestones.
class GoalStore {
public:
    struct Config {
        std::string    db_path;                 // <root>/memory/goals.db
        MemoryCortex*  episodic_sink = nullptr; // optional; nullptr = no mirror
    };

    explicit GoalStore(Config cfg);
    ~GoalStore();
    GoalStore(const GoalStore&)            = delete;
    GoalStore& operator=(const GoalStore&) = delete;

    // ── creation / mutation ────────────────────────────────────────────────

    // Insert. Status is forced to Proposed regardless of the input value;
    // created_at and last_progress_at are stamped to "now". Returns the new id
    // or 0 on failure.
    std::int64_t propose(Goal g);

    // Status transition. Returns false if `to` is not reachable from current
    // status, or the goal does not exist. `note` is included in the episodic
    // mirror payload (e.g. "blocked: waiting on libfvad fix").
    bool setStatus(std::int64_t id, GoalStatus to, std::string note = "");

    // Convenience wrappers — all enforce isValidTransition.
    bool accept   (std::int64_t id, std::string note = "")
                                                { return setStatus(id, GoalStatus::Accepted,   std::move(note)); }
    bool start    (std::int64_t id, std::string note = "")
                                                { return setStatus(id, GoalStatus::InProgress, std::move(note)); }
    bool complete (std::int64_t id, std::string note = "")
                                                { return setStatus(id, GoalStatus::Done,       std::move(note)); }
    bool reject   (std::int64_t id, std::string note = "")
                                                { return setStatus(id, GoalStatus::Rejected,   std::move(note)); }
    bool cancel   (std::int64_t id, std::string note = "")
                                                { return setStatus(id, GoalStatus::Cancelled,  std::move(note)); }

    // Block: status → Blocked, blocked_reason set. Idempotent: re-blocking
    // updates the reason without changing created_at.
    bool block    (std::int64_t id, std::string reason);

    // Unblock: status → InProgress, blocked_reason cleared. Refuses if not
    // currently Blocked.
    bool unblock  (std::int64_t id, std::string note = "");

    // Field-level updates that don't transition status.
    bool setPlanJson    (std::int64_t id, std::string plan_json);
    bool setPriority    (std::int64_t id, int priority);
    bool setDeadline    (std::int64_t id, std::optional<GoalTimestamp> deadline_at);
    bool setSummary     (std::int64_t id, std::string summary);
    bool setTags        (std::int64_t id, std::vector<std::string> tags);
    bool bumpProgress   (std::int64_t id);   // refresh last_progress_at to now

    // ── reads ──────────────────────────────────────────────────────────────

    std::optional<Goal> get(std::int64_t id) const;
    std::vector<Goal>   list(const GoalQuery& q) const;

    // Direct children (parent_id == id). Sorted by created_at ascending.
    std::vector<Goal>   children(std::int64_t parent_id) const;

    // Walk parent_id chain root → ... → id. Empty if id has no parent.
    // Detects cycles (truncates with a warning).
    std::vector<Goal>   ancestors(std::int64_t id) const;

    // BFS subtree rooted at root_id, including root_id itself first.
    std::vector<Goal>   subtree(std::int64_t root_id) const;

    // Goals with deadline_at within `horizon` of now, status not terminal.
    std::vector<Goal>   dueSoon(std::chrono::seconds horizon) const;

    // InProgress goals whose last_progress_at is older than `threshold`.
    std::vector<Goal>   staleInProgress(std::chrono::seconds threshold) const;

    // All currently-Blocked goals.
    std::vector<Goal>   blocked() const;

    // Counts by status for monitoring + tests.
    int                 countByStatus(GoalStatus s) const;
    int                 totalCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace arise
