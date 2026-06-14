// Phase 6 commit 1: Suggestion + SuggestionGate + FeedbackDb + ProactiveEngine.
//
// Engine tests publish events on a real Blackboard and assert that the
// evaluator builds the right tier + persists. Gate tests are time-injected
// for determinism.

#include "blackboard/blackboard.hpp"
#include "cortex/feedback_db.hpp"
#include "cortex/proactive.hpp"
#include "cortex/suggestion.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

std::string mkSandbox(const std::string& tag) {
    auto base = fs::temp_directory_path()
              / ("arise_p6c1_test_" + tag + "_"
                 + std::to_string(::getpid()) + "_"
                 + std::to_string(duration_cast<microseconds>(
                       system_clock::now().time_since_epoch()).count()));
    fs::create_directories(base);
    return base.string();
}

arise::FeedbackDb::Config feedbackCfg(const std::string& dir) {
    arise::FeedbackDb::Config c;
    c.db_path = dir + "/feedback.db";
    return c;
}

arise::Suggestion mkSuggestion(arise::Tier t, std::string cat, std::string text) {
    arise::Suggestion s;
    s.tier     = t;
    s.category = std::move(cat);
    s.text     = std::move(text);
    return s;
}

system_clock::time_point makeLocalHour(int hour) {
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    tm_buf.tm_hour = hour;
    tm_buf.tm_min  = 0;
    tm_buf.tm_sec  = 0;
    return system_clock::from_time_t(mktime(&tm_buf));
}

} // namespace

// ─── tier enum bridge ──────────────────────────────────────────────────────

TEST(SuggestionTier, RoundTrip) {
    using arise::Tier;
    for (auto t : {Tier::Silent, Tier::Ambient, Tier::Active, Tier::Urgent}) {
        auto s = arise::tierToString(t);
        auto back = arise::tierFromString(s);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, t);
    }
    EXPECT_FALSE(arise::tierFromString("garbage").has_value());
    EXPECT_LT(arise::tierRank(Tier::Silent),  arise::tierRank(Tier::Ambient));
    EXPECT_LT(arise::tierRank(Tier::Ambient), arise::tierRank(Tier::Active));
    EXPECT_LT(arise::tierRank(Tier::Active),  arise::tierRank(Tier::Urgent));
}

// ─── SuggestionGate ────────────────────────────────────────────────────────

TEST(SuggestionGate, SilentAlwaysBlocked) {
    arise::SuggestionGate gate(arise::SuggestionGate::Config{});
    auto s = mkSuggestion(arise::Tier::Silent, "test", "x");
    EXPECT_EQ(gate.check(s), arise::GateOutcome::BlockedSilent);
}

TEST(SuggestionGate, RateLimitsAmbientPerInterval) {
    arise::SuggestionGate::Config c;
    c.min_interval_ambient = 60s;
    arise::SuggestionGate gate(c);
    auto s = mkSuggestion(arise::Tier::Ambient, "battery", "low");
    auto t0 = system_clock::now();
    EXPECT_EQ(gate.check(s, t0), arise::GateOutcome::Pass);
    gate.noteFired(arise::Tier::Ambient, t0);
    EXPECT_EQ(gate.check(s, t0 + 30s), arise::GateOutcome::BlockedRateLimit);
    EXPECT_EQ(gate.check(s, t0 + 61s), arise::GateOutcome::Pass);
}

TEST(SuggestionGate, UrgentSlipsThroughAlways) {
    arise::SuggestionGate::Config c;
    c.min_interval_urgent  = 0s;       // intentionally unrestricted
    c.quiet_hours_enabled  = true;
    c.quiet_start_hour     = 0;
    c.quiet_end_hour       = 23;       // huge quiet window
    arise::SuggestionGate gate(c);
    auto s = mkSuggestion(arise::Tier::Urgent, "alarm", "siren");
    auto t = makeLocalHour(3);          // dead of night
    EXPECT_EQ(gate.check(s, t), arise::GateOutcome::Pass);
    gate.noteFired(arise::Tier::Urgent, t);
    EXPECT_EQ(gate.check(s, t + 1ms), arise::GateOutcome::Pass);
}

TEST(SuggestionGate, QuietHoursBlocksAmbientButNotUrgent) {
    arise::SuggestionGate::Config c;
    c.quiet_hours_enabled = true;
    c.quiet_start_hour    = 23;
    c.quiet_end_hour      = 7;
    arise::SuggestionGate gate(c);

    auto night = makeLocalHour(2);
    auto noon  = makeLocalHour(12);

    auto a = mkSuggestion(arise::Tier::Ambient, "drift", "x");
    auto u = mkSuggestion(arise::Tier::Urgent,  "alarm", "x");

    EXPECT_EQ(gate.check(a, night), arise::GateOutcome::BlockedQuietHours);
    EXPECT_EQ(gate.check(a, noon),  arise::GateOutcome::Pass);
    EXPECT_EQ(gate.check(u, night), arise::GateOutcome::Pass);
}

TEST(SuggestionGate, QuietHoursNonWrapping) {
    arise::SuggestionGate::Config c;
    c.quiet_hours_enabled = true;
    c.quiet_start_hour    = 13;
    c.quiet_end_hour      = 14;
    arise::SuggestionGate gate(c);
    EXPECT_TRUE (gate.isInQuietHours(makeLocalHour(13)));
    EXPECT_FALSE(gate.isInQuietHours(makeLocalHour(14)));
    EXPECT_FALSE(gate.isInQuietHours(makeLocalHour(12)));
}

TEST(SuggestionGate, ConsecutiveRejectsAutoMutes) {
    arise::SuggestionGate::Config c;
    c.mute_after_rejects = 3;
    c.mute_window        = 1h;
    arise::SuggestionGate gate(c);

    auto s = mkSuggestion(arise::Tier::Ambient, "stuck", "still stuck?");
    EXPECT_EQ(gate.check(s), arise::GateOutcome::Pass);

    gate.setConsecutiveRejects("stuck", 3);
    EXPECT_EQ(gate.check(s), arise::GateOutcome::BlockedCategoryMuted);
}

TEST(SuggestionGate, ManualMuteAndClear) {
    arise::SuggestionGate gate(arise::SuggestionGate::Config{});
    auto s = mkSuggestion(arise::Tier::Active, "noisy", "x");
    EXPECT_EQ(gate.check(s), arise::GateOutcome::Pass);
    gate.muteCategory("noisy", 1h);
    EXPECT_EQ(gate.check(s), arise::GateOutcome::BlockedCategoryMuted);
    gate.muteCategory("noisy", 0s);
    EXPECT_EQ(gate.check(s), arise::GateOutcome::Pass);
}

TEST(SuggestionGate, MuteDoesNotApplyToUrgent) {
    arise::SuggestionGate gate(arise::SuggestionGate::Config{});
    gate.muteCategory("alarm_heard", 1h);
    auto u = mkSuggestion(arise::Tier::Urgent, "alarm_heard", "fire");
    EXPECT_EQ(gate.check(u), arise::GateOutcome::Pass);
}

// ─── FeedbackDb ────────────────────────────────────────────────────────────

TEST(FeedbackDb, RecordProposedAssignsId) {
    auto dir = mkSandbox("fb_propose");
    arise::FeedbackDb fb(feedbackCfg(dir));
    auto s = mkSuggestion(arise::Tier::Ambient, "battery_warning", "low");
    auto id = fb.recordProposed(s);
    EXPECT_GT(id, 0);
    EXPECT_EQ(s.id, id);
    auto row = fb.get(id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->category, "battery_warning");
    EXPECT_EQ(row->decision, arise::Decision::Pending);
}

TEST(FeedbackDb, DecisionTransitionsAndIsTerminal) {
    auto dir = mkSandbox("fb_decide");
    arise::FeedbackDb fb(feedbackCfg(dir));
    auto s = mkSuggestion(arise::Tier::Ambient, "x", "y");
    auto id = fb.recordProposed(s);
    EXPECT_TRUE (fb.recordDecision(id, arise::Decision::Accepted));
    EXPECT_FALSE(fb.recordDecision(id, arise::Decision::Rejected));   // terminal
    auto row = fb.get(id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->decision, arise::Decision::Accepted);
    EXPECT_TRUE(row->decided_at.has_value());
}

TEST(FeedbackDb, ConsecutiveRejectsCounts) {
    auto dir = mkSandbox("fb_streak");
    arise::FeedbackDb fb(feedbackCfg(dir));

    auto reject = [&](){
        auto s = mkSuggestion(arise::Tier::Ambient, "noisy", "x");
        auto id = fb.recordProposed(s);
        std::this_thread::sleep_for(1100ms);  // bump proposed_at to next second
        fb.recordDecision(id, arise::Decision::Rejected);
    };
    auto accept = [&](){
        auto s = mkSuggestion(arise::Tier::Ambient, "noisy", "x");
        auto id = fb.recordProposed(s);
        std::this_thread::sleep_for(1100ms);
        fb.recordDecision(id, arise::Decision::Accepted);
    };

    reject(); reject();
    EXPECT_EQ(fb.consecutiveRejects("noisy"), 2);
    accept();
    EXPECT_EQ(fb.consecutiveRejects("noisy"), 0);
    EXPECT_EQ(fb.categoryCount("noisy", arise::Decision::Rejected), 2);
    EXPECT_EQ(fb.categoryCount("noisy", arise::Decision::Accepted), 1);
}

TEST(FeedbackDb, TimeoutPendingFlipsOldRows) {
    auto dir = mkSandbox("fb_timeout");
    arise::FeedbackDb fb(feedbackCfg(dir));

    auto s = mkSuggestion(arise::Tier::Ambient, "old", "x");
    fb.recordProposed(s);
    std::this_thread::sleep_for(1100ms);
    int n = fb.timeoutPending(0s);   // anything older than now
    EXPECT_EQ(n, 1);
    auto row = fb.get(s.id);
    EXPECT_EQ(row->decision, arise::Decision::Timedout);
}

// ─── candidate builders (pure) ─────────────────────────────────────────────

TEST(BuildCandidate, WatcherNoticeMapsSeverity) {
    auto urg  = arise::ProactiveEngine::buildCandidate(
        "agent.watcher.notice",
        nlohmann::json{{"kind","battery_critical"},{"severity","urgent"},{"summary","plug in now"}});
    EXPECT_EQ(urg.tier, arise::Tier::Urgent);
    EXPECT_EQ(urg.category, "battery_critical");

    auto act  = arise::ProactiveEngine::buildCandidate(
        "agent.watcher.notice",
        nlohmann::json{{"kind","visible_error"},{"severity","active"},{"summary","error on screen"}});
    EXPECT_EQ(act.tier, arise::Tier::Active);

    auto amb  = arise::ProactiveEngine::buildCandidate(
        "agent.watcher.notice",
        nlohmann::json{{"kind","battery_warning"},{"severity","ambient"},{"summary","plug in soon"}});
    EXPECT_EQ(amb.tier, arise::Tier::Ambient);
}

TEST(BuildCandidate, GoalEventsClassify) {
    auto due = arise::ProactiveEngine::buildCandidate(
        "goal.due", nlohmann::json{{"summary","cmake refactor"}});
    EXPECT_EQ(due.tier, arise::Tier::Ambient);
    EXPECT_EQ(due.category, "goal_due");
    EXPECT_NE(due.text.find("cmake refactor"), std::string::npos);

    auto esc = arise::ProactiveEngine::buildCandidate(
        "goal.escalated", nlohmann::json{{"summary","ship feature"}});
    EXPECT_EQ(esc.tier, arise::Tier::Active);
    EXPECT_EQ(esc.category, "goal_escalated");

    auto stale = arise::ProactiveEngine::buildCandidate(
        "goal.stale",
        nlohmann::json{{"summary","review docs"},{"stale_seconds","600000"}});
    EXPECT_EQ(stale.tier, arise::Tier::Ambient);
    EXPECT_EQ(stale.category, "goal_stale");
    EXPECT_NE(stale.text.find("review docs"), std::string::npos);
}

TEST(BuildCandidate, AudioScenesClassify) {
    EXPECT_EQ(arise::ProactiveEngine::buildCandidate(
        "audio.scene_changed", nlohmann::json{{"scene","alarm"}}).tier,
        arise::Tier::Urgent);
    EXPECT_EQ(arise::ProactiveEngine::buildCandidate(
        "audio.scene_changed", nlohmann::json{{"scene","doorbell"}}).tier,
        arise::Tier::Active);
    EXPECT_EQ(arise::ProactiveEngine::buildCandidate(
        "audio.scene_changed", nlohmann::json{{"scene","music"}}).text,
        "");      // ignored — Silent default + empty text
}

TEST(BuildCandidate, UnknownTopicYieldsNothing) {
    auto s = arise::ProactiveEngine::buildCandidate(
        "vision.first_frame", nlohmann::json{{"hash",1}});
    EXPECT_TRUE(s.text.empty());
}

// ─── ProactiveEngine end-to-end ────────────────────────────────────────────

TEST(ProactiveEngine, EvaluateBuildsAndPersists) {
    auto dir = mkSandbox("pe_basic");
    arise::FeedbackDb fb(feedbackCfg(dir));
    arise::Blackboard bb;

    arise::ProactiveEngine::Config pc;
    pc.bb = &bb;
    pc.feedback = &fb;
    arise::ProactiveEngine eng(pc);

    auto sub = bb.subscribe("proactive.suggestion");
    auto r = eng.evaluate(
        "agent.watcher.notice",
        nlohmann::json{{"kind","battery_critical"},
                        {"severity","urgent"},
                        {"summary","plug in now"}});
    EXPECT_TRUE(r.published);
    EXPECT_EQ(r.outcome, arise::GateOutcome::Pass);
    EXPECT_GT(r.suggestion.id, 0);
    EXPECT_EQ(r.suggestion.tier, arise::Tier::Urgent);

    auto ev = sub.next(200ms);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->payload.value("category", std::string{}), "battery_critical");
    EXPECT_EQ(ev->payload.value("tier", std::string{}), "urgent");

    auto row = fb.get(r.suggestion.id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->decision, arise::Decision::Pending);
}

TEST(ProactiveEngine, RateLimitedSecondAmbientDropped) {
    auto dir = mkSandbox("pe_rate");
    arise::FeedbackDb fb(feedbackCfg(dir));
    arise::Blackboard bb;

    arise::ProactiveEngine::Config pc;
    pc.bb = &bb;
    pc.feedback = &fb;
    pc.gate.min_interval_ambient = 60s;
    arise::ProactiveEngine eng(pc);

    auto first  = eng.evaluate("goal.due",
        nlohmann::json{{"summary","x"}});
    EXPECT_TRUE(first.published);

    auto second = eng.evaluate("goal.due",
        nlohmann::json{{"summary","y"}});
    EXPECT_FALSE(second.published);
    EXPECT_EQ(second.outcome, arise::GateOutcome::BlockedRateLimit);

    auto stats = eng.stats();
    EXPECT_EQ(stats.passed, 1u);
    EXPECT_EQ(stats.blocked_rate, 1u);
}

TEST(ProactiveEngine, FeedbackStreakAutoMutes) {
    auto dir = mkSandbox("pe_streak");
    arise::FeedbackDb fb(feedbackCfg(dir));
    arise::Blackboard bb;

    arise::ProactiveEngine::Config pc;
    pc.bb = &bb;
    pc.feedback = &fb;
    pc.gate.min_interval_ambient = 0s;        // ignore rate limit for this test
    pc.gate.mute_after_rejects   = 2;
    pc.gate.mute_window          = 1h;
    arise::ProactiveEngine eng(pc);

    auto fire_and_reject = [&]() {
        auto r = eng.evaluate("goal.due", nlohmann::json{{"summary","x"}});
        std::this_thread::sleep_for(1100ms);  // ensure proposed_at differs
        fb.recordDecision(r.suggestion.id, arise::Decision::Rejected);
    };

    fire_and_reject();
    fire_and_reject();
    auto third = eng.evaluate("goal.due", nlohmann::json{{"summary","x"}});
    EXPECT_FALSE(third.published);
    EXPECT_EQ(third.outcome, arise::GateOutcome::BlockedCategoryMuted);
}

TEST(ProactiveEngine, WorkerThreadPicksUpEvents) {
    auto dir = mkSandbox("pe_thread");
    arise::FeedbackDb fb(feedbackCfg(dir));
    arise::Blackboard bb;

    arise::ProactiveEngine::Config pc;
    pc.bb = &bb;
    pc.feedback = &fb;
    arise::ProactiveEngine eng(pc);

    auto sub = bb.subscribe("proactive.suggestion");
    eng.start();

    bb.publish("agent.watcher.notice",
        nlohmann::json{{"kind","alarm_heard"},
                        {"severity","urgent"},
                        {"summary","alarm sound"}});
    auto ev = sub.next(500ms);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->payload.value("tier", std::string{}), "urgent");
    eng.stop();
}

TEST(ProactiveEngine, SilentTopicDoesNothing) {
    auto dir = mkSandbox("pe_silent");
    arise::FeedbackDb fb(feedbackCfg(dir));
    arise::Blackboard bb;

    arise::ProactiveEngine::Config pc;
    pc.bb = &bb;
    pc.feedback = &fb;
    arise::ProactiveEngine eng(pc);

    auto r = eng.evaluate("vision.first_frame", nlohmann::json{{"hash",1}});
    EXPECT_FALSE(r.published);
    EXPECT_EQ(eng.stats().suggested, 0u);
}
