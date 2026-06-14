#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace arise {

class Blackboard;
class GoalStore;

// Background polling + event-driven goal awakener.
//
// Runs one worker thread that:
//   * drains `idle.left` events from the blackboard
//   * every `tick_interval` ticks, scans the GoalStore for:
//       - deadlines within `due_horizon`             → goal.due
//       - deadlines within `escalate_horizon` (≪ due) → goal.escalated
//       - in_progress goals stale for > `stale_threshold` → goal.stale
//   * on boot (start()), fires goal.resumed for every in-progress goal
//   * on each idle.left, fires goal.resumed for every in-progress goal
//
// Deduping: per-(goal_id, event_type) "last fired" timestamp keeps the bus
// from being flooded. due/escalated/stale/resumed each have their own
// configurable renotify period.
//
// Thread-safety: Blackboard and GoalStore must outlive the scheduler.
// stop() is idempotent and is called from the destructor.
class GoalScheduler {
public:
    struct Config {
        GoalStore*    store = nullptr;     // required
        Blackboard*   bb    = nullptr;     // required

        std::chrono::seconds tick_interval     { 60 };
        std::chrono::seconds due_horizon       { 30 * 60 };       // 30 min
        std::chrono::seconds escalate_horizon  {  5 * 60 };       //  5 min
        std::chrono::seconds stale_threshold   { 7 * 24 * 3600 }; //  7 d
        std::chrono::seconds due_renotify      {     60 * 60 };   //  1 h
        std::chrono::seconds escalate_renotify {     10 * 60 };   // 10 min
        std::chrono::seconds stale_renotify    { 24 * 3600 };     //  1 d
        std::chrono::seconds resume_renotify   {     30 * 60 };   // 30 min

        bool resume_on_boot      = true;
        bool resume_on_idle_left = true;
    };

    struct Stats {
        std::size_t scans            = 0;
        std::size_t due_fires        = 0;
        std::size_t escalated_fires  = 0;
        std::size_t stale_fires      = 0;
        std::size_t resumed_fires    = 0;
        std::size_t idle_lefts_seen  = 0;
    };

    explicit GoalScheduler(Config cfg);
    ~GoalScheduler();
    GoalScheduler(const GoalScheduler&)            = delete;
    GoalScheduler& operator=(const GoalScheduler&) = delete;

    void start();
    void stop();
    bool running() const;

    Stats stats() const;

    // For tests / CLI: forces an immediate scan on the calling thread,
    // bypassing the tick loop. Safe to call before start().
    void scanNow();

    // For tests / CLI: forces a resume sweep on the calling thread.
    void resumeNow(const std::string& trigger = "manual");

private:
    struct Impl;
    std::unique_ptr<Impl> p_;

    void workerLoop_();
    void runScan_();
    void runResume_(const std::string& trigger);
};

} // namespace arise
