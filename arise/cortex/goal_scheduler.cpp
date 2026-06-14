#include "cortex/goal_scheduler.hpp"

#include "blackboard/blackboard.hpp"
#include "cortex/goals.hpp"
#include "util/log.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

// Composite key for the dedup table: (goal_id, event_kind).
struct DedupKey {
    std::int64_t  goal_id;
    int           kind;           // 0=due, 1=escalated, 2=stale, 3=resumed
    bool operator==(const DedupKey& o) const {
        return goal_id == o.goal_id && kind == o.kind;
    }
};
struct DedupKeyHash {
    std::size_t operator()(const DedupKey& k) const {
        return std::hash<std::int64_t>{}(k.goal_id) ^
               (std::hash<int>{}(k.kind) << 1);
    }
};

constexpr int kKindDue       = 0;
constexpr int kKindEscalated = 1;
constexpr int kKindStale     = 2;
constexpr int kKindResumed   = 3;

const char* kindToTopic(int kind) {
    switch (kind) {
        case kKindDue:       return "goal.due";
        case kKindEscalated: return "goal.escalated";
        case kKindStale:     return "goal.stale";
        case kKindResumed:   return "goal.resumed";
    }
    return "goal.unknown";
}

json renderGoalPayload(const Goal& g, int kind, const std::string& extra_key,
                       const std::string& extra_val) {
    json p;
    p["goal_id"]  = g.id;
    p["summary"]  = g.summary;
    p["status"]   = toString(g.status);
    p["priority"] = g.priority;
    if (g.deadline_at) {
        p["deadline_epoch"] = duration_cast<seconds>(
                                  g.deadline_at->time_since_epoch()).count();
    }
    p["last_progress_epoch"] = duration_cast<seconds>(
                                   g.last_progress_at.time_since_epoch()).count();
    if (!g.tags.empty()) p["tags"] = g.tags;
    if (g.parent_id)     p["parent_id"] = *g.parent_id;
    if (kind == kKindStale && !g.blocked_reason.empty())
        p["blocked_reason"] = g.blocked_reason;
    if (!extra_key.empty()) p[extra_key] = extra_val;
    return p;
}

} // namespace

// ─── impl ──────────────────────────────────────────────────────────────────

struct GoalScheduler::Impl {
    Config cfg;

    std::atomic<bool> running { false };
    std::atomic<bool> stopping{ false };

    std::mutex                stop_mu;
    std::condition_variable   stop_cv;

    std::thread worker;

    Blackboard::Subscription idle_sub;

    mutable std::mutex stats_mu;
    Stats              stats;

    // (goal_id, event_kind) → last fired time (steady_clock).
    std::mutex dedup_mu;
    std::unordered_map<DedupKey, steady_clock::time_point, DedupKeyHash> last_fired;

    bool sleepFor(milliseconds ms) {
        std::unique_lock<std::mutex> lk(stop_mu);
        return stop_cv.wait_for(lk, ms, [this] { return stopping.load(); });
    }

    seconds renotifyForKind(int kind) const {
        switch (kind) {
            case kKindDue:       return cfg.due_renotify;
            case kKindEscalated: return cfg.escalate_renotify;
            case kKindStale:     return cfg.stale_renotify;
            case kKindResumed:   return cfg.resume_renotify;
        }
        return seconds{0};
    }

    // Returns true if we should fire this event (and stamps the dedup map).
    bool shouldFire(std::int64_t id, int kind) {
        auto now    = steady_clock::now();
        auto period = renotifyForKind(kind);
        std::lock_guard<std::mutex> lk(dedup_mu);
        DedupKey k{ id, kind };
        auto it = last_fired.find(k);
        if (it != last_fired.end() && now - it->second < period) return false;
        last_fired[k] = now;
        return true;
    }

    void incStat(int kind) {
        std::lock_guard<std::mutex> lk(stats_mu);
        switch (kind) {
            case kKindDue:       ++stats.due_fires;       break;
            case kKindEscalated: ++stats.escalated_fires; break;
            case kKindStale:     ++stats.stale_fires;     break;
            case kKindResumed:   ++stats.resumed_fires;   break;
        }
    }
};

GoalScheduler::GoalScheduler(Config cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = std::move(cfg);
    if (!p_->cfg.store || !p_->cfg.bb) {
        log::error("GoalScheduler: store and bb are required");
    }
}

GoalScheduler::~GoalScheduler() { stop(); }

bool GoalScheduler::running() const { return p_->running.load(); }

GoalScheduler::Stats GoalScheduler::stats() const {
    std::lock_guard<std::mutex> lk(p_->stats_mu);
    return p_->stats;
}

void GoalScheduler::start() {
    if (!p_->cfg.store || !p_->cfg.bb) return;
    bool expected = false;
    if (!p_->running.compare_exchange_strong(expected, true)) return;
    p_->stopping.store(false);

    if (p_->cfg.resume_on_idle_left) {
        p_->idle_sub = p_->cfg.bb->subscribe("idle.left");
    }

    p_->worker = std::thread(&GoalScheduler::workerLoop_, this);
    log::info("GoalScheduler: started");
}

void GoalScheduler::stop() {
    if (!p_) return;
    bool was_running = p_->running.exchange(false);
    {
        std::lock_guard<std::mutex> lk(p_->stop_mu);
        p_->stopping.store(true);
    }
    p_->stop_cv.notify_all();
    if (p_->idle_sub.valid()) p_->idle_sub.stop();
    if (p_->worker.joinable()) p_->worker.join();
    if (was_running) log::info("GoalScheduler: stopped");
}

void GoalScheduler::workerLoop_() {
    if (p_->cfg.resume_on_boot) runResume_("boot");

    while (!p_->stopping.load()) {
        // Drain idle.left events that arrive within this tick window. We
        // block on next() up to the time remaining until the next scheduled
        // scan; on timeout we run the scan and start a new tick.
        auto tick_end = steady_clock::now()
                      + duration_cast<milliseconds>(p_->cfg.tick_interval);

        while (!p_->stopping.load()) {
            auto now = steady_clock::now();
            if (now >= tick_end) break;
            auto remaining = duration_cast<milliseconds>(tick_end - now);
            if (!p_->idle_sub.valid()) {
                if (p_->sleepFor(remaining)) return;
                break;
            }
            auto ev = p_->idle_sub.next(remaining);
            if (!ev) break;
            { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.idle_lefts_seen; }
            if (ev->topic == "idle.left" && p_->cfg.resume_on_idle_left) {
                runResume_("idle.left");
            }
        }
        if (p_->stopping.load()) break;
        runScan_();
    }
}

void GoalScheduler::scanNow() {
    if (p_->cfg.store && p_->cfg.bb) runScan_();
}

void GoalScheduler::resumeNow(const std::string& trigger) {
    if (p_->cfg.store && p_->cfg.bb) runResume_(trigger);
}

void GoalScheduler::runScan_() {
    auto& cfg = p_->cfg;
    { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.scans; }

    // Deadline buckets: escalated (within escalate_horizon) is the strongest
    // signal; due (within due_horizon) is the broader bucket. Each goal lands
    // in at most one bucket per scan.
    auto due_rows = cfg.store->dueSoon(cfg.due_horizon);
    for (const auto& g : due_rows) {
        if (!g.deadline_at) continue;
        auto seconds_left = duration_cast<seconds>(*g.deadline_at - system_clock::now());
        int  kind = (seconds_left <= cfg.escalate_horizon)
                        ? kKindEscalated : kKindDue;
        if (!p_->shouldFire(g.id, kind)) continue;
        json payload = renderGoalPayload(g, kind, "seconds_until_deadline",
                                         std::to_string(seconds_left.count()));
        cfg.bb->publish(kindToTopic(kind), payload);
        p_->incStat(kind);
    }

    auto stale_rows = cfg.store->staleInProgress(cfg.stale_threshold);
    for (const auto& g : stale_rows) {
        if (!p_->shouldFire(g.id, kKindStale)) continue;
        auto idle_for = duration_cast<seconds>(
                            system_clock::now() - g.last_progress_at);
        json payload = renderGoalPayload(g, kKindStale, "stale_seconds",
                                         std::to_string(idle_for.count()));
        cfg.bb->publish(kindToTopic(kKindStale), payload);
        p_->incStat(kKindStale);
    }
}

void GoalScheduler::runResume_(const std::string& trigger) {
    auto& cfg = p_->cfg;
    GoalQuery q;
    q.status = GoalStatus::InProgress;
    q.limit  = 1000;
    auto rows = cfg.store->list(q);
    for (const auto& g : rows) {
        if (!p_->shouldFire(g.id, kKindResumed)) continue;
        json payload = renderGoalPayload(g, kKindResumed, "trigger", trigger);
        cfg.bb->publish(kindToTopic(kKindResumed), payload);
        p_->incStat(kKindResumed);
    }
}

} // namespace arise
