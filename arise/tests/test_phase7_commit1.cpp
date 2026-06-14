// Phase 7 commit 1: DeviceStore + FederationRouter.
//
// Token hashing + permission checks + handler dispatch are all
// deterministic; tests don't need a network listener (commit 2's job).

#include "blackboard/blackboard.hpp"
#include "cortex/device_store.hpp"
#include "cortex/federation_router.hpp"
#include "cortex/feedback_db.hpp"
#include "cortex/goals.hpp"
#include "cortex/memory_cortex.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

std::string mkSandbox(const std::string& tag) {
    auto base = fs::temp_directory_path()
              / ("arise_p7c1_test_" + tag + "_"
                 + std::to_string(::getpid()) + "_"
                 + std::to_string(duration_cast<microseconds>(
                       system_clock::now().time_since_epoch()).count()));
    fs::create_directories(base);
    return base.string();
}

arise::DeviceStore::Config storeCfg(const std::string& dir) {
    arise::DeviceStore::Config c;
    c.path = dir + "/devices.json";
    return c;
}

arise::MemoryCortex::Config sandboxCortexCfg(const std::string& dir) {
    arise::MemoryCortex::Config c;
    c.root             = dir;
    c.embed_cache_path = dir + "/embed_cache.sqlite";
    c.decay_thread     = false;
    return c;
}

arise::FeedbackDb::Config feedbackCfg(const std::string& dir) {
    arise::FeedbackDb::Config c;
    c.db_path = dir + "/feedback.db";
    return c;
}

arise::GoalStore::Config goalsCfg(const std::string& dir,
                                  arise::MemoryCortex* sink = nullptr) {
    arise::GoalStore::Config c;
    c.db_path       = dir + "/goals.db";
    c.episodic_sink = sink;
    return c;
}

} // namespace

// ─── token hashing helpers ─────────────────────────────────────────────────

TEST(DeviceTokens, Sha256IsStableHex64) {
    auto a = arise::DeviceStore::sha256Hex("hello");
    auto b = arise::DeviceStore::sha256Hex("hello");
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.size(), 64u);
    for (char c : a) EXPECT_TRUE(std::isxdigit(c));
    EXPECT_NE(a, arise::DeviceStore::sha256Hex("hellp"));
}

TEST(DeviceTokens, ConstantTimeEquals) {
    EXPECT_TRUE (arise::DeviceStore::constantTimeEquals("abc", "abc"));
    EXPECT_FALSE(arise::DeviceStore::constantTimeEquals("abc", "abd"));
    EXPECT_FALSE(arise::DeviceStore::constantTimeEquals("abc", "abcd"));
    EXPECT_TRUE (arise::DeviceStore::constantTimeEquals("",    ""));
}

TEST(DeviceTokens, RandomTokenIsHexAndUnique) {
    auto a = arise::DeviceStore::randomTokenHex(32);
    auto b = arise::DeviceStore::randomTokenHex(32);
    EXPECT_EQ(a.size(), 64u);
    EXPECT_EQ(b.size(), 64u);
    EXPECT_NE(a, b);
    for (char c : a) EXPECT_TRUE(std::isxdigit(c));
}

// ─── DeviceStore round-trip + auth ────────────────────────────────────────

TEST(DeviceStoreRoundTrip, AddSavesAndReloads) {
    auto dir = mkSandbox("ds_round");
    {
        arise::DeviceStore s(storeCfg(dir));
        auto added = s.addDevice("phone-aurelius",
                                 arise::DeviceKind::Phone);
        ASSERT_TRUE(added.has_value());
        EXPECT_EQ(added->plaintext_token.size(), 64u);
        EXPECT_EQ(added->device.kind, arise::DeviceKind::Phone);
        EXPECT_FALSE(added->device.token_sha256_hex.empty());
        EXPECT_EQ(added->device.token_sha256_hex,
                  arise::DeviceStore::sha256Hex(added->plaintext_token));
    }
    arise::DeviceStore s2(storeCfg(dir));
    auto rows = s2.list();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].name, "phone-aurelius");
}

TEST(DeviceStoreAuth, GetByTokenAcceptsRightTokenOnly) {
    auto dir = mkSandbox("ds_auth");
    arise::DeviceStore s(storeCfg(dir));
    auto a1 = s.addDevice("dev_a", arise::DeviceKind::Phone);
    auto a2 = s.addDevice("dev_b", arise::DeviceKind::Tablet);
    ASSERT_TRUE(a1 && a2);

    auto by_token1 = s.getByToken(a1->plaintext_token);
    ASSERT_TRUE(by_token1.has_value());
    EXPECT_EQ(by_token1->id, a1->device.id);

    auto by_token2 = s.getByToken(a2->plaintext_token);
    ASSERT_TRUE(by_token2.has_value());
    EXPECT_EQ(by_token2->id, a2->device.id);

    EXPECT_FALSE(s.getByToken("not_a_real_token").has_value());
    EXPECT_FALSE(s.getByToken("").has_value());
}

TEST(DeviceStoreAuth, RecordSeenBumpsCounters) {
    auto dir = mkSandbox("ds_seen");
    arise::DeviceStore s(storeCfg(dir));
    auto a = s.addDevice("phone");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->device.event_count, 0);
    EXPECT_TRUE(s.recordSeen(a->device.id));
    EXPECT_TRUE(s.recordSeen(a->device.id));
    auto fresh = s.getById(a->device.id);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(fresh->event_count, 2);
    EXPECT_NE(fresh->last_seen.time_since_epoch().count(), 0);
}

TEST(DeviceStoreAuth, RevokeRemovesRecord) {
    auto dir = mkSandbox("ds_revoke");
    arise::DeviceStore s(storeCfg(dir));
    auto a = s.addDevice("phone");
    ASSERT_TRUE(a);
    EXPECT_TRUE(s.revokeById(a->device.id));
    EXPECT_FALSE(s.getById(a->device.id).has_value());
    EXPECT_FALSE(s.getByToken(a->plaintext_token).has_value());
    EXPECT_FALSE(s.revokeById(a->device.id));    // already gone
}

// ─── FederationRouter ingest paths ─────────────────────────────────────────

TEST(FederationRouter, RejectsBadToken) {
    auto dir = mkSandbox("fr_badtoken");
    arise::DeviceStore  devices(storeCfg(dir));
    arise::Blackboard   bb;

    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb;
    arise::FederationRouter router(rc);

    auto resp = router.ingest(nlohmann::json{
        {"type","ping"}, {"payload",{}},
    }, "wrong_token");
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.code, arise::FederationRouter::Code::Unauthorized);
}

TEST(FederationRouter, RejectsMismatchedSourceDevice) {
    auto dir = mkSandbox("fr_source");
    arise::DeviceStore devices(storeCfg(dir));
    auto a = devices.addDevice("phone");
    ASSERT_TRUE(a);
    arise::Blackboard bb;
    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb;
    arise::FederationRouter router(rc);

    auto resp = router.ingest(nlohmann::json{
        {"type","ping"},
        {"source_device","forged-id"},
    }, a->plaintext_token);
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.code, arise::FederationRouter::Code::Forbidden);
}

TEST(FederationRouter, RejectsRevokedToken) {
    auto dir = mkSandbox("fr_revoked");
    arise::DeviceStore devices(storeCfg(dir));
    auto a = devices.addDevice("phone");
    ASSERT_TRUE(a);
    EXPECT_TRUE(devices.revokeById(a->device.id));
    arise::Blackboard bb;
    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb;
    arise::FederationRouter router(rc);

    auto resp = router.ingest(nlohmann::json{{"type","ping"}},
                              a->plaintext_token);
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.code, arise::FederationRouter::Code::Unauthorized);
}

TEST(FederationRouter, RejectsBadJson) {
    auto dir = mkSandbox("fr_badjson");
    arise::DeviceStore  devices(storeCfg(dir));
    auto a = devices.addDevice("phone");
    ASSERT_TRUE(a);
    arise::Blackboard bb;
    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb;
    arise::FederationRouter router(rc);
    auto resp = router.ingestRaw("{not-json}", a->plaintext_token);
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.code, arise::FederationRouter::Code::BadRequest);
}

TEST(FederationRouter, PingPongs) {
    auto dir = mkSandbox("fr_ping");
    arise::DeviceStore devices(storeCfg(dir));
    auto a = devices.addDevice("phone");
    ASSERT_TRUE(a);
    arise::Blackboard bb;
    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb;
    arise::FederationRouter router(rc);

    auto sub = bb.subscribe("federation.ping");
    auto resp = router.ingest(nlohmann::json{{"type","ping"}},
                              a->plaintext_token);
    EXPECT_TRUE(resp.ok);
    EXPECT_TRUE(resp.payload.value("pong", false));
    auto ev = sub.next(200ms);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->payload.value("source_device", std::string{}),
              a->device.id);
}

TEST(FederationRouter, UtterancePublishesAndMirrors) {
    auto dir = mkSandbox("fr_utter");
    arise::DeviceStore  devices(storeCfg(dir));
    auto a = devices.addDevice("phone");
    ASSERT_TRUE(a);
    arise::Blackboard   bb;
    arise::MemoryCortex cortex(sandboxCortexCfg(dir));

    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb; rc.cortex = &cortex;
    arise::FederationRouter router(rc);

    auto sub = bb.subscribe("federation.utterance");
    auto resp = router.ingest(nlohmann::json{
        {"type","utterance"},
        {"payload",{
            {"text", "what was I supposed to do today"},
            {"modality", "voice"}}},
    }, a->plaintext_token);
    EXPECT_TRUE(resp.ok);

    auto ev = sub.next(200ms);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->payload.value("text", std::string{}),
              "what was I supposed to do today");
    EXPECT_EQ(ev->payload.value("source_device", std::string{}),
              a->device.id);

    auto recent = cortex.recentEvents(10);
    bool found = false;
    for (const auto& e : recent) {
        if (e.kind == "federation_utterance"
            && e.summary.find("supposed to do today") != std::string::npos) {
            found = true;
            EXPECT_NE(e.summary.find(a->device.id), std::string::npos);
        }
    }
    EXPECT_TRUE(found);

    auto fresh = devices.getById(a->device.id);
    EXPECT_EQ(fresh->event_count, 1);
}

TEST(FederationRouter, UtteranceRequiresPermission) {
    auto dir = mkSandbox("fr_utter_perm");
    arise::DeviceStore devices(storeCfg(dir));
    arise::DevicePermissions p;
    p.can_utterance = false;
    auto a = devices.addDevice("display", arise::DeviceKind::Overlay, p);
    ASSERT_TRUE(a);
    arise::Blackboard bb;
    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb;
    arise::FederationRouter router(rc);

    auto resp = router.ingest(nlohmann::json{
        {"type","utterance"},
        {"payload",{{"text","hi"}}},
    }, a->plaintext_token);
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.code, arise::FederationRouter::Code::Forbidden);
}

TEST(FederationRouter, DecisionFlipsFeedbackDb) {
    auto dir = mkSandbox("fr_decision");
    arise::DeviceStore devices(storeCfg(dir));
    auto a = devices.addDevice("phone");
    ASSERT_TRUE(a);
    arise::Blackboard bb;
    arise::FeedbackDb fb(feedbackCfg(dir));

    arise::Suggestion s;
    s.tier = arise::Tier::Ambient;
    s.category = "battery_warning";
    s.text = "low";
    auto sid = fb.recordProposed(s);
    ASSERT_GT(sid, 0);

    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb; rc.feedback = &fb;
    arise::FederationRouter router(rc);

    auto resp = router.ingest(nlohmann::json{
        {"type","decision"},
        {"payload",{{"id", sid}, {"decision","accepted"}}},
    }, a->plaintext_token);
    EXPECT_TRUE(resp.ok);

    auto row = fb.get(sid);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->decision, arise::Decision::Accepted);
}

TEST(FederationRouter, DecisionRejectsBadFields) {
    auto dir = mkSandbox("fr_decision_bad");
    arise::DeviceStore devices(storeCfg(dir));
    auto a = devices.addDevice("phone");
    ASSERT_TRUE(a);
    arise::Blackboard bb;
    arise::FeedbackDb fb(feedbackCfg(dir));

    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb; rc.feedback = &fb;
    arise::FederationRouter router(rc);

    auto missing_id = router.ingest(nlohmann::json{
        {"type","decision"}, {"payload",{{"decision","accepted"}}},
    }, a->plaintext_token);
    EXPECT_FALSE(missing_id.ok);

    auto bad_decision = router.ingest(nlohmann::json{
        {"type","decision"},
        {"payload",{{"id", 999}, {"decision","pending"}}},
    }, a->plaintext_token);
    EXPECT_FALSE(bad_decision.ok);
}

TEST(FederationRouter, GoalQueryReturnsInProgressGoals) {
    auto dir = mkSandbox("fr_goalquery");
    arise::DeviceStore  devices(storeCfg(dir));
    auto a = devices.addDevice("phone");
    ASSERT_TRUE(a);
    arise::Blackboard   bb;
    arise::MemoryCortex cortex(sandboxCortexCfg(dir));
    arise::GoalStore    goals(goalsCfg(dir));

    {
        arise::Goal g; g.summary = "in flight"; g.priority = 70;
        auto id = goals.propose(g);
        goals.accept(id); goals.start(id);
    }
    {
        arise::Goal g; g.summary = "blocked-out"; g.priority = 50;
        auto id = goals.propose(g);
        goals.accept(id); goals.complete(id);   // terminal — must not appear
    }

    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb; rc.cortex = &cortex; rc.goals = &goals;
    arise::FederationRouter router(rc);

    auto resp = router.ingest(nlohmann::json{
        {"type","goal_query"},
        {"payload",{{"limit", 10}}},
    }, a->plaintext_token);
    EXPECT_TRUE(resp.ok);
    ASSERT_TRUE(resp.payload.contains("goals"));
    auto& gs = resp.payload["goals"];
    ASSERT_TRUE(gs.is_array());
    EXPECT_EQ(gs.size(), 1u);
    EXPECT_EQ(gs[0].value("summary", std::string{}), "in flight");
    EXPECT_EQ(gs[0].value("status",  std::string{}), "in_progress");
}

TEST(FederationRouter, ClockSkewBlocksBadTimestamps) {
    auto dir = mkSandbox("fr_skew");
    arise::DeviceStore devices(storeCfg(dir));
    auto a = devices.addDevice("phone");
    ASSERT_TRUE(a);
    arise::Blackboard bb;
    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb;
    rc.clock_skew_tolerance = 60s;
    arise::FederationRouter router(rc);

    long long now = duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();
    auto resp = router.ingest(nlohmann::json{
        {"type","ping"},
        {"ts", now - 10000},
    }, a->plaintext_token);
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.code, arise::FederationRouter::Code::BadRequest);
}

TEST(FederationRouter, OversizedEventRejected) {
    auto dir = mkSandbox("fr_big");
    arise::DeviceStore devices(storeCfg(dir));
    auto a = devices.addDevice("phone");
    ASSERT_TRUE(a);
    arise::Blackboard bb;
    arise::FederationRouter::Config rc;
    rc.devices = &devices; rc.bb = &bb;
    rc.max_event_bytes = 256;
    arise::FederationRouter router(rc);

    nlohmann::json evt;
    evt["type"]    = "utterance";
    evt["payload"] = nlohmann::json{{"text", std::string(1024, 'x')}};
    auto resp = router.ingest(evt, a->plaintext_token);
    EXPECT_FALSE(resp.ok);
    EXPECT_EQ(resp.code, arise::FederationRouter::Code::BadRequest);
}
