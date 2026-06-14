// Phase 3 commit 2: GoalScheduler — drives the GoalStore from the blackboard.
//
// Each test owns a fresh GoalStore + Blackboard pair; we call scanNow() and
// resumeNow() directly to keep timing deterministic. The worker thread is
// only exercised in the StopSemantics + IdleLeftDrivesResume tests where it
// matters.

#include "blackboard/blackboard.hpp"
#include "cortex/goal_scheduler.hpp"
#include "cortex/goals.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

std::string mkSandbox(const std::string& tag) {
    auto base = fs::temp_directory_path()
              / ("arise_sched_test_" + tag + "_"
                 + std::to_string(::getpid()) + "_"
                 + std::to_string(duration_cast<microseconds>(
                       system_clock::now().time_since_epoch()).count()));
    fs::create_directories(base);
    return base.string();
}

arise::GoalStore::Config storeCfg(const std::string& dir) {
    arise::GoalStore::Config c;
    c.db_path = dir + "/goals.db";
    return c;
}

arise::GoalScheduler::Config schedCfg(arise::GoalStore* store,
                                      arise::Blackboard* bb) {
    arise::GoalScheduler::Config c;
    c.store = store;
    c.bb    = bb;
    // Tight cadence so the worker thread doesn't dominate test runtime.
    c.tick_interval     = duration_cast<seconds>(milliseconds(50));
    c.due_horizon       = hours(1);
    c.escalate_horizon  = minutes(5);
    c.stale_threshold   = seconds(0);   // anything older than now → stale
    c.due_renotify      = seconds(60);
    c.escalate_renotify = seconds(60);
    c.stale_renotify    = seconds(60);
    c.resume_renotify   = seconds(60);
    c.resume_on_boot      = false;       // tests opt in explicitly
    c.resume_on_idle_left = false;
    return c;
}

std::int64_t proposeAndStart(arise::GoalStore& s, const std::string& summary) {
    arise::Goal g; g.summary = summary;
    auto id = s.propose(g);
    EXPECT_TRUE(s.accept(id));
    EXPECT_TRUE(s.start (id));
    return id;
}

// Drain all events of a given topic from a wildcard subscription, with
// brief retries so the publishing thread has a chance to flush.
std::vector<arise::BlackboardEvent>
collect(arise::Blackboard::Subscription& sub, milliseconds total) {
    std::vector<arise::BlackboardEvent> out;
    auto deadline = steady_clock::now() + total;
    while (steady_clock::now() < deadline) {
        auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
        auto ev = sub.next(remaining);
        if (!ev) break;
        out.push_back(*ev);
    }
    auto extra = sub.drain();
    out.insert(out.end(), extra.begin(), extra.end());
    return out;
}

int countTopic(const std::vector<arise::BlackboardEvent>& evs,
               const std::string& topic) {
    int n = 0;
    for (auto& e : evs) if (e.topic == topic) ++n;
    return n;
}

} // namespace

// ─── due / escalated split ──────────────────────────────────────────────────

TEST(GoalSchedulerScan, FiresDueAndEscalatedAccordingToHorizon) {
    auto dir = mkSandbox("due");
    arise::GoalStore   store(storeCfg(dir));
    arise::Blackboard  bb;
    arise::GoalScheduler sched(schedCfg(&store, &bb));

    // 30-min deadline → falls in due_horizon (1h) but outside escalate (5m).
    arise::Goal far; far.summary = "wide due"; far.deadline_at = system_clock::now() + minutes(30);
    auto far_id = store.propose(far);
    store.accept(far_id);

    // 2-min deadline → escalated bucket.
    arise::Goal soon; soon.summary = "imminent"; soon.deadline_at = system_clock::now() + minutes(2);
    auto soon_id = store.propose(soon);
    store.accept(soon_id);
    store.start (soon_id);

    auto sub = bb.subscribe("");
    sched.scanNow();
    auto evs = collect(sub, 200ms);

    EXPECT_EQ(countTopic(evs, "goal.due"),       1);
    EXPECT_EQ(countTopic(evs, "goal.escalated"), 1);

    // Confirm payload routing: due event refers to the far goal.
    for (auto& e : evs) {
        if (e.topic == "goal.due") {
            EXPECT_EQ(e.payload.value("goal_id", 0), far_id);
        } else if (e.topic == "goal.escalated") {
            EXPECT_EQ(e.payload.value("goal_id", 0), soon_id);
        }
    }
}

TEST(GoalSchedulerScan, RenotifyDedupBlocksSecondScan) {
    auto dir = mkSandbox("renotify");
    arise::GoalStore   store(storeCfg(dir));
    arise::Blackboard  bb;
    auto cfg = schedCfg(&store, &bb);
    cfg.due_renotify      = hours(1);
    cfg.escalate_renotify = hours(1);
    arise::GoalScheduler sched(cfg);

    arise::Goal g; g.summary = "x"; g.deadline_at = system_clock::now() + minutes(15);
    store.propose(g);

    auto sub = bb.subscribe("");
    sched.scanNow();
    sched.scanNow();           // second scan within the renotify window
    sched.scanNow();
    auto evs = collect(sub, 200ms);
    EXPECT_EQ(countTopic(evs, "goal.due"), 1);
}

TEST(GoalSchedulerScan, SkipsTerminalGoals) {
    auto dir = mkSandbox("terminal");
    arise::GoalStore   store(storeCfg(dir));
    arise::Blackboard  bb;
    arise::GoalScheduler sched(schedCfg(&store, &bb));

    arise::Goal g; g.summary = "completed early"; g.deadline_at = system_clock::now() + minutes(1);
    auto id = store.propose(g);
    store.accept(id);
    store.complete(id);

    auto sub = bb.subscribe("");
    sched.scanNow();
    auto evs = collect(sub, 200ms);
    EXPECT_EQ(countTopic(evs, "goal.due"),       0);
    EXPECT_EQ(countTopic(evs, "goal.escalated"), 0);
}

// ─── stale ──────────────────────────────────────────────────────────────────

TEST(GoalSchedulerScan, FiresStaleForStaleInProgress) {
    auto dir = mkSandbox("stale");
    arise::GoalStore   store(storeCfg(dir));
    arise::Blackboard  bb;
    arise::GoalScheduler sched(schedCfg(&store, &bb));   // stale_threshold = 0s

    proposeAndStart(store, "languishing");
    std::this_thread::sleep_for(1100ms);  // cross the 1-second tick

    auto sub = bb.subscribe("");
    sched.scanNow();
    auto evs = collect(sub, 200ms);
    ASSERT_EQ(countTopic(evs, "goal.stale"), 1);
    for (auto& e : evs) {
        if (e.topic == "goal.stale") {
            EXPECT_TRUE(e.payload.contains("stale_seconds"));
        }
    }
}

TEST(GoalSchedulerScan, ProgressBumpClearsStale) {
    auto dir = mkSandbox("stale_clear");
    arise::GoalStore   store(storeCfg(dir));
    arise::Blackboard  bb;
    auto cfg = schedCfg(&store, &bb);
    cfg.stale_renotify = seconds(0);  // allow re-fire on every scan
    arise::GoalScheduler sched(cfg);

    auto id = proposeAndStart(store, "still moving");
    std::this_thread::sleep_for(1100ms);

    auto sub = bb.subscribe("");
    sched.scanNow();
    auto evs1 = collect(sub, 100ms);
    EXPECT_GE(countTopic(evs1, "goal.stale"), 1);

    EXPECT_TRUE(store.bumpProgress(id));
    cfg.stale_threshold = hours(1);  // can't reconfigure live; build a fresh one
    arise::GoalScheduler sched2(cfg);
    sched2.scanNow();
    auto evs2 = collect(sub, 100ms);
    EXPECT_EQ(countTopic(evs2, "goal.stale"), 0);
}

// ─── resume ─────────────────────────────────────────────────────────────────

TEST(GoalSchedulerResume, ResumeNowFiresOnceForEachInProgress) {
    auto dir = mkSandbox("resume");
    arise::GoalStore   store(storeCfg(dir));
    arise::Blackboard  bb;
    arise::GoalScheduler sched(schedCfg(&store, &bb));

    proposeAndStart(store, "alpha");
    proposeAndStart(store, "beta");
    proposeAndStart(store, "gamma");
    // proposed but not started — should NOT receive resume.
    arise::Goal idle; idle.summary = "idle"; store.propose(idle);

    auto sub = bb.subscribe("");
    sched.resumeNow("test");
    auto evs = collect(sub, 200ms);
    EXPECT_EQ(countTopic(evs, "goal.resumed"), 3);
    for (auto& e : evs) {
        if (e.topic == "goal.resumed") {
            EXPECT_EQ(e.payload.value("trigger", std::string("")), "test");
            EXPECT_EQ(e.payload.value("status", std::string("")), "in_progress");
        }
    }
}

TEST(GoalSchedulerResume, ResumeRenotifyDedups) {
    auto dir = mkSandbox("resume_dedup");
    arise::GoalStore   store(storeCfg(dir));
    arise::Blackboard  bb;
    auto cfg = schedCfg(&store, &bb);
    cfg.resume_renotify = hours(1);
    arise::GoalScheduler sched(cfg);

    proposeAndStart(store, "only one resume");

    auto sub = bb.subscribe("");
    sched.resumeNow("first");
    sched.resumeNow("second");
    sched.resumeNow("third");
    auto evs = collect(sub, 100ms);
    EXPECT_EQ(countTopic(evs, "goal.resumed"), 1);
}

// ─── thread + idle.left wiring ──────────────────────────────────────────────

TEST(GoalSchedulerThread, IdleLeftDrivesResume) {
    auto dir = mkSandbox("idle");
    arise::GoalStore   store(storeCfg(dir));
    arise::Blackboard  bb;
    auto cfg = schedCfg(&store, &bb);
    cfg.tick_interval     = seconds(60);   // long — we want idle.left, not the timer
    cfg.resume_on_idle_left = true;
    arise::GoalScheduler sched(cfg);

    proposeAndStart(store, "react to idle leave");

    auto sub = bb.subscribe("goal.resumed");
    sched.start();

    // Without the published idle.left, no resume should fire (resume_on_boot is false).
    EXPECT_FALSE(sub.next(150ms).has_value());

    bb.publish("idle.left", nlohmann::json{{"idle_ms", 12345}});
    auto ev = sub.next(500ms);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->topic, "goal.resumed");
    EXPECT_EQ(ev->payload.value("trigger", std::string("")), "idle.left");

    sched.stop();
    EXPECT_FALSE(sched.running());
}

TEST(GoalSchedulerThread, BootResumeFiresOnce) {
    auto dir = mkSandbox("boot");
    arise::GoalStore   store(storeCfg(dir));
    arise::Blackboard  bb;
    auto cfg = schedCfg(&store, &bb);
    cfg.resume_on_boot = true;
    arise::GoalScheduler sched(cfg);

    proposeAndStart(store, "fire on boot");
    auto sub = bb.subscribe("goal.resumed");
    sched.start();

    auto ev = sub.next(500ms);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->payload.value("trigger", std::string("")), "boot");

    sched.stop();
}

TEST(GoalSchedulerThread, StopIsIdempotent) {
    auto dir = mkSandbox("stop");
    arise::GoalStore   store(storeCfg(dir));
    arise::Blackboard  bb;
    arise::GoalScheduler sched(schedCfg(&store, &bb));
    sched.start();
    EXPECT_TRUE(sched.running());
    sched.stop();
    sched.stop();
    EXPECT_FALSE(sched.running());
}
