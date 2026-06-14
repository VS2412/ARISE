// Phase 4 commit 1: SubAgent + Watcher + Curator + Orchestrator.
//
// Most tests are deterministic (no Ollama). The end-to-end Watcher worker
// tests publish events on a local Blackboard and verify the agent.* topic
// payload. Curator's structured-output is exercised via a synthetic LLM
// response — we test parseFacts + the upsert side effect, not Ollama.

#include "blackboard/blackboard.hpp"
#include "cortex/curator.hpp"
#include "cortex/goals.hpp"
#include "cortex/memory_cortex.hpp"
#include "cortex/orchestrator.hpp"
#include "cortex/sub_agent.hpp"
#include "cortex/watcher.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

std::string mkSandbox(const std::string& tag) {
    auto base = fs::temp_directory_path()
              / ("arise_p4_test_" + tag + "_"
                 + std::to_string(::getpid()) + "_"
                 + std::to_string(duration_cast<microseconds>(
                       system_clock::now().time_since_epoch()).count()));
    fs::create_directories(base);
    return base.string();
}

arise::MemoryCortex::Config sandboxCortexCfg(const std::string& dir) {
    arise::MemoryCortex::Config c;
    c.root             = dir;
    c.embed_cache_path = dir + "/embed_cache.sqlite";
    c.decay_thread     = false;
    return c;
}

arise::GoalStore::Config sandboxGoalsCfg(const std::string& dir,
                                         arise::MemoryCortex* sink = nullptr) {
    arise::GoalStore::Config c;
    c.db_path       = dir + "/goals.db";
    c.episodic_sink = sink;
    return c;
}

} // namespace

// ─── SubAgent helpers (pure, no Ollama) ─────────────────────────────────────

TEST(SubAgentParse, FirstJsonObjectFindsBalancedBlock) {
    auto blob = arise::SubAgent::firstJsonObject(R"(prefix {"a": 1, "b": {"c": 2}} trailing)");
    ASSERT_TRUE(blob.has_value());
    EXPECT_EQ(*blob, R"({"a": 1, "b": {"c": 2}})");
}

TEST(SubAgentParse, FirstJsonObjectIgnoresBracesInStrings) {
    auto blob = arise::SubAgent::firstJsonObject(R"({"k":"a}b{c"})");
    ASSERT_TRUE(blob.has_value());
    EXPECT_EQ(*blob, R"({"k":"a}b{c"})");
}

TEST(SubAgentParse, FirstJsonObjectReturnsNulloptOnNoBraces) {
    EXPECT_FALSE(arise::SubAgent::firstJsonObject("hello world").has_value());
    EXPECT_FALSE(arise::SubAgent::firstJsonObject("").has_value());
}

TEST(SubAgentParse, StripsThinkingBlocks) {
    auto out = arise::SubAgent::stripThinkingBlocks(
        "<think>step one</think>visible<think>step two</think>tail");
    EXPECT_EQ(out, "visibletail");
}

TEST(SubAgentParse, StripsThinkingDropsUnterminated) {
    auto out = arise::SubAgent::stripThinkingBlocks(
        "before<think>open with no close");
    EXPECT_EQ(out, "before");
}

TEST(SubAgentRun, RejectsEmptyTask) {
    arise::SubAgent::Config c;
    c.role = "test";
    c.timeout_sec = 1;
    arise::SubAgent a(c);
    auto r = a.run("");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "empty task");
}

TEST(SubAgentRun, UnreachableOllamaSurfacesError) {
    arise::SubAgent::Config c;
    c.role         = "test";
    c.ollama_url   = "http://127.0.0.1:1";   // reserved port → connection refused
    c.timeout_sec  = 2;
    c.connect_timeout_sec = 1;
    arise::SubAgent a(c);
    auto r = a.run("anything");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.reachable);
    EXPECT_FALSE(r.error.empty());
}

// ─── Watcher pure rules ─────────────────────────────────────────────────────

TEST(WatcherRules, BatteryThresholds) {
    arise::Blackboard bb;
    arise::Watcher::Config wc; wc.bb = &bb;
    arise::Watcher w(wc);

    auto d = w.evaluateBatteryPct(80);
    EXPECT_EQ(d.severity, arise::Watcher::Severity::Silent);

    d = w.evaluateBatteryPct(20);
    EXPECT_EQ(d.severity, arise::Watcher::Severity::Ambient);
    EXPECT_EQ(d.kind, "battery_warning");
    EXPECT_FALSE(d.propose_goal);

    d = w.evaluateBatteryPct(5);
    EXPECT_EQ(d.severity, arise::Watcher::Severity::Urgent);
    EXPECT_EQ(d.kind, "battery_critical");
    // propose_goal is true only when goals store is wired; our wc.goals is null.
    EXPECT_FALSE(d.propose_goal);
}

TEST(WatcherRules, BatteryThresholdsProposeWhenGoalsWired) {
    auto dir = mkSandbox("battery_goals");
    arise::Blackboard bb;
    arise::GoalStore  goals(sandboxGoalsCfg(dir));

    arise::Watcher::Config wc;
    wc.bb    = &bb;
    wc.goals = &goals;
    arise::Watcher w(wc);

    auto d = w.evaluateBatteryPct(5);
    EXPECT_TRUE(d.propose_goal);
    EXPECT_FALSE(d.goal_summary.empty());
}

TEST(WatcherRules, CaptionKeywordDetection) {
    arise::Blackboard bb;
    arise::Watcher::Config wc; wc.bb = &bb;
    arise::Watcher w(wc);

    auto d = w.evaluateCaption("the screen shows a calm desktop background");
    EXPECT_EQ(d.severity, arise::Watcher::Severity::Silent);

    d = w.evaluateCaption("a red Traceback (most recent call last) appears");
    EXPECT_EQ(d.severity, arise::Watcher::Severity::Active);
    EXPECT_EQ(d.kind, "visible_error");

    d = w.evaluateCaption("HTTP 404 not found in the browser tab");
    EXPECT_EQ(d.severity, arise::Watcher::Severity::Active);
}

TEST(WatcherRules, AudioSceneRouting) {
    arise::Blackboard bb;
    arise::Watcher::Config wc; wc.bb = &bb;
    arise::Watcher w(wc);

    EXPECT_EQ(w.evaluateAudioScene("alarm").severity,    arise::Watcher::Severity::Urgent);
    EXPECT_EQ(w.evaluateAudioScene("doorbell").severity, arise::Watcher::Severity::Active);
    EXPECT_EQ(w.evaluateAudioScene("phone").severity,    arise::Watcher::Severity::Active);
    EXPECT_EQ(w.evaluateAudioScene("speech").severity,   arise::Watcher::Severity::Silent);
    EXPECT_EQ(w.evaluateAudioScene("music").severity,    arise::Watcher::Severity::Silent);
    EXPECT_EQ(w.evaluateAudioScene("silence").severity,  arise::Watcher::Severity::Silent);
}

// ─── Watcher worker thread end-to-end ──────────────────────────────────────

TEST(WatcherWorker, BatteryDeltaTriggersNotice) {
    arise::Blackboard bb;
    arise::Watcher::Config wc; wc.bb = &bb;
    arise::Watcher w(wc);
    auto sub = bb.subscribe("agent.watcher.notice");
    w.start();

    bb.publish("system.delta", nlohmann::json{{"battery_pct", 8}});
    auto ev = sub.next(500ms);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->payload.value("kind", std::string("")), "battery_critical");
    EXPECT_EQ(ev->payload.value("severity", std::string("")), "urgent");
    w.stop();
}

TEST(WatcherWorker, CaptionWithErrorTriggersNotice) {
    arise::Blackboard bb;
    arise::Watcher::Config wc; wc.bb = &bb;
    arise::Watcher w(wc);
    auto sub = bb.subscribe("agent.watcher.notice");
    w.start();

    bb.publish("vision.caption", nlohmann::json{
        {"caption", "an error dialog from the IDE"},
    });
    auto ev = sub.next(500ms);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->payload.value("kind", std::string("")), "visible_error");
    w.stop();
}

TEST(WatcherWorker, AudioAlarmTriggersUrgent) {
    arise::Blackboard bb;
    arise::Watcher::Config wc; wc.bb = &bb;
    arise::Watcher w(wc);
    auto sub = bb.subscribe("agent.watcher.notice");
    w.start();

    bb.publish("audio.scene_changed", nlohmann::json{{"scene", "alarm"}});
    auto ev = sub.next(500ms);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->payload.value("severity", std::string("")), "urgent");
    EXPECT_EQ(ev->payload.value("kind",     std::string("")), "alarm_heard");
    w.stop();
}

TEST(WatcherWorker, SilentSignalDoesNotEmit) {
    arise::Blackboard bb;
    arise::Watcher::Config wc; wc.bb = &bb;
    arise::Watcher w(wc);
    auto sub = bb.subscribe("agent.watcher.notice");
    w.start();

    bb.publish("system.delta", nlohmann::json{{"battery_pct", 88}});  // high
    bb.publish("audio.scene_changed", nlohmann::json{{"scene", "music"}});
    EXPECT_FALSE(sub.next(150ms).has_value());
    w.stop();
}

TEST(WatcherWorker, CriticalBatteryProposesGoal) {
    auto dir = mkSandbox("battery_propose");
    arise::Blackboard bb;
    arise::GoalStore  goals(sandboxGoalsCfg(dir));

    arise::Watcher::Config wc;
    wc.bb    = &bb;
    wc.goals = &goals;
    arise::Watcher w(wc);
    auto sub = bb.subscribe("agent.watcher.notice");
    w.start();

    bb.publish("system.delta", nlohmann::json{{"battery_pct", 4}});
    auto ev = sub.next(500ms);
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(ev->payload.contains("goal_id"));
    auto goal_id = ev->payload["goal_id"].get<std::int64_t>();
    auto g = goals.get(goal_id);
    ASSERT_TRUE(g.has_value());
    EXPECT_NE(g->summary.find("plug in"), std::string::npos);
    EXPECT_GE(g->priority, 80);
    w.stop();
}

TEST(WatcherWorker, StopIsIdempotent) {
    arise::Blackboard bb;
    arise::Watcher::Config wc; wc.bb = &bb;
    arise::Watcher w(wc);
    w.start();
    EXPECT_TRUE(w.running());
    w.stop();
    w.stop();
    EXPECT_FALSE(w.running());
}

// ─── Curator parser + upsert path ──────────────────────────────────────────

TEST(CuratorParse, ExtractsValidFacts) {
    std::string llm = R"({"facts":[
        {"subject":"user","predicate":"has_dog","object":"Mochi","confidence":0.95},
        {"subject":"user","predicate":"likes","object":"coffee","confidence":0.8}
    ]})";
    auto facts = arise::Curator::parseFacts(llm, 10);
    ASSERT_EQ(facts.size(), 2u);
    EXPECT_EQ(facts[0].subject,    "user");
    EXPECT_EQ(facts[0].predicate,  "has_dog");
    EXPECT_EQ(facts[0].object,     "Mochi");
    EXPECT_NEAR(facts[0].confidence, 0.95, 1e-9);
}

TEST(CuratorParse, StripsMarkdownAndThinking) {
    std::string llm =
        "<think>I should extract facts.</think>\n"
        "```json\n"
        "{\"facts\":[{\"subject\":\"user\",\"predicate\":\"likes\","
        "\"object\":\"vim\",\"confidence\":0.7}]}\n"
        "```";
    auto facts = arise::Curator::parseFacts(llm, 10);
    ASSERT_EQ(facts.size(), 1u);
    EXPECT_EQ(facts[0].object, "vim");
}

TEST(CuratorParse, RejectsIncompleteTriples) {
    std::string llm = R"({"facts":[
        {"subject":"","predicate":"likes","object":"x","confidence":0.9},
        {"subject":"user","predicate":"","object":"y","confidence":0.9},
        {"subject":"user","predicate":"likes","object":"","confidence":0.9},
        {"subject":"user","predicate":"likes","object":"valid","confidence":0.9}
    ]})";
    auto facts = arise::Curator::parseFacts(llm, 10);
    ASSERT_EQ(facts.size(), 1u);
    EXPECT_EQ(facts[0].object, "valid");
}

TEST(CuratorParse, ClampsConfidenceAndCapsCount) {
    std::string llm = R"({"facts":[
        {"subject":"u","predicate":"is","object":"a","confidence":1.7},
        {"subject":"u","predicate":"is","object":"b","confidence":-0.3},
        {"subject":"u","predicate":"is","object":"c","confidence":0.5},
        {"subject":"u","predicate":"is","object":"d","confidence":0.5}
    ]})";
    auto facts = arise::Curator::parseFacts(llm, 2);
    ASSERT_EQ(facts.size(), 2u);
    EXPECT_NEAR(facts[0].confidence, 1.0, 1e-9);
    EXPECT_NEAR(facts[1].confidence, 0.0, 1e-9);
}

TEST(CuratorParse, NoFactsKeyYieldsEmpty) {
    EXPECT_TRUE(arise::Curator::parseFacts("nope", 5).empty());
    EXPECT_TRUE(arise::Curator::parseFacts(R"({"other": []})", 5).empty());
    EXPECT_TRUE(arise::Curator::parseFacts(R"({"facts": "not an array"})", 5).empty());
}

// ─── Orchestrator lifecycle ────────────────────────────────────────────────

TEST(OrchestratorLifecycle, StartStopWithWatcherOnly) {
    arise::Blackboard bb;
    arise::Orchestrator::Config oc;
    oc.bb = &bb;
    oc.enable_watcher = true;
    oc.enable_curator = false;            // no cortex configured
    arise::Orchestrator o(oc);
    o.start();
    EXPECT_TRUE(o.running());
    EXPECT_NE(o.watcher(), nullptr);
    EXPECT_EQ(o.curator(), nullptr);
    o.stop();
    EXPECT_FALSE(o.running());
}

TEST(OrchestratorLifecycle, FullStackWithCortex) {
    auto dir = mkSandbox("orch");
    arise::Blackboard   bb;
    arise::MemoryCortex cortex(sandboxCortexCfg(dir));
    arise::GoalStore    goals(sandboxGoalsCfg(dir, &cortex));

    arise::Orchestrator::Config oc;
    oc.bb     = &bb;
    oc.goals  = &goals;
    oc.cortex = &cortex;
    oc.curator_llm.ollama_url = "http://127.0.0.1:1";  // unreachable; lifecycle test only
    arise::Orchestrator o(oc);
    o.start();
    EXPECT_NE(o.watcher(),    nullptr);
    EXPECT_NE(o.curator(),    nullptr);
    EXPECT_NE(o.curatorLlm(), nullptr);
    o.stop();
    o.stop();   // idempotent
    EXPECT_FALSE(o.running());
}
