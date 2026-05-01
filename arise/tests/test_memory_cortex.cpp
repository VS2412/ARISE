// Tier 1 — round-trip + invariant tests for MemoryCortex and IdentityStore.
//
// Every test runs against a fresh temp directory so they're independent and
// can be run in parallel. Vec/Ollama dependencies are sidestepped: the mood,
// recall, decay and contradiction tests use sqlite_vec_path="" (FTS-only) and
// decay_thread=false. The one network-dependent test (EmbedCacheDedup) probes
// Ollama and skips itself if unreachable, so the suite is green on a build
// box without Ollama running.

#include "cortex/identity.hpp"
#include "cortex/memory_cortex.hpp"
#include "util/embedding_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace arise;
using namespace std::chrono;

namespace {

std::string makeTempDir(const std::string& tag) {
    auto base  = fs::temp_directory_path();
    auto stamp = duration_cast<microseconds>(
                     system_clock::now().time_since_epoch()).count();
    auto p = base / ("arise_test_" + tag + "_" + std::to_string(stamp));
    fs::create_directories(p);
    return p.string();
}

class TempRoot {
public:
    explicit TempRoot(const std::string& tag) : path_(makeTempDir(tag)) {}
    ~TempRoot() { std::error_code ec; fs::remove_all(path_, ec); }
    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

MemoryCortex::Config defaultCfg(const std::string& root) {
    MemoryCortex::Config c;
    c.root                    = root;
    c.embed_cache_path        = root + "/embed_cache.db";
    c.sqlite_vec_path         = "";              // FTS-only; no vec0.so dependency
    c.decay_thread            = false;            // tests drive purges/ticks themselves
    c.mood_half_life_seconds  = 60.0 * 60.0 * 24.0 * 365.0;  // 1y — no incidental drift
    return c;
}

} // namespace

// 1 ─ Smoke: write/read round-trip, ts auto-stamped, salience auto-heuristic fires.
TEST(MemoryCortexTest, Smoke) {
    TempRoot tmp("smoke");
    MemoryCortex mc(defaultCfg(tmp.path()));

    EpisodicEvent ev;
    ev.kind    = "conversation_turn";
    ev.summary = "user said hello and asked about the weather";

    int64_t id = mc.recordEvent(ev);
    ASSERT_GT(id, 0);

    auto got = mc.getEvent(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->kind, "conversation_turn");
    EXPECT_EQ(got->summary, "user said hello and asked about the weather");
    EXPECT_GT(got->salience, 0.0);                  // auto-heuristic kicked in
    EXPECT_GT(got->ts.time_since_epoch().count(), 0);
}

// 2 ─ Embed cache: identical text doesn't grow the cache. Skips when offline.
TEST(MemoryCortexTest, EmbedCacheDedup) {
    TempRoot tmp("embed_cache");

    EmbeddingClient::Config c;
    c.cache_path = tmp.path() + "/cache.db";
    EmbeddingClient ec(c);

    if (!ec.probe()) {
        GTEST_SKIP() << "Ollama unreachable; skipping embed cache test";
    }

    int n0 = ec.cacheSize();
    auto v1 = ec.embed("the quick brown fox jumps over the lazy dog");
    ASSERT_FALSE(v1.empty());
    EXPECT_EQ(ec.cacheSize(), n0 + 1);

    auto v2 = ec.embed("the quick brown fox jumps over the lazy dog");
    ASSERT_EQ(v1.size(), v2.size());
    EXPECT_EQ(v1, v2);
    EXPECT_EQ(ec.cacheSize(), n0 + 1);              // no new row
}

// 3 ─ Salience-based decay: low salience + past decay_at gets purged; high stays.
TEST(MemoryCortexTest, SalienceDecayPurge) {
    TempRoot tmp("decay");
    MemoryCortex mc(defaultCfg(tmp.path()));

    EpisodicEvent low;
    low.kind     = "ambient_noise";
    low.summary  = "fan whirred for a bit";
    low.salience = 0.05;
    low.decay_at = system_clock::now() - hours(1);  // already past
    int64_t low_id = mc.recordEvent(low);
    ASSERT_GT(low_id, 0);

    EpisodicEvent high;
    high.kind     = "summary";
    high.summary  = "user finalised quarterly plan";
    high.salience = 0.95;                            // permanent (no decay_at)
    int64_t high_id = mc.recordEvent(high);
    ASSERT_GT(high_id, 0);

    int purged = mc.purgeDecayed();
    EXPECT_EQ(purged, 1);
    EXPECT_FALSE(mc.getEvent(low_id).has_value());
    EXPECT_TRUE (mc.getEvent(high_id).has_value());
}

// 4 ─ Contradiction-aware semantic upsert: weaker contradictions are rejected,
// stronger ones overwrite in place. Reinforce-on-match also exercised.
TEST(MemoryCortexTest, SemanticContradictionResolution) {
    TempRoot tmp("contradict");
    MemoryCortex mc(defaultCfg(tmp.path()));

    auto mk = [](std::string subj, std::string pred, std::string obj, double conf) {
        SemanticFact f;
        f.subject    = std::move(subj);
        f.predicate  = std::move(pred);
        f.object     = std::move(obj);
        f.confidence = conf;
        return f;
    };

    int64_t id1 = mc.upsertFact(mk("user", "favourite_editor", "vim",   0.70));
    EXPECT_GT(id1, 0);
    {
        auto facts = mc.queryFacts("user", "favourite_editor");
        ASSERT_EQ(facts.size(), 1u);
        EXPECT_EQ(facts[0].object, "vim");
    }

    // weaker contradiction → rejected; row unchanged
    int64_t id2 = mc.upsertFact(mk("user", "favourite_editor", "emacs", 0.60));
    EXPECT_EQ(id2, 0);
    {
        auto facts = mc.queryFacts("user", "favourite_editor");
        ASSERT_EQ(facts.size(), 1u);
        EXPECT_EQ(facts[0].object, "vim");
    }

    // stronger contradiction → overwrites in place (same row id)
    int64_t id3 = mc.upsertFact(mk("user", "favourite_editor", "emacs", 0.95));
    EXPECT_EQ(id3, id1);
    {
        auto facts = mc.queryFacts("user", "favourite_editor");
        ASSERT_EQ(facts.size(), 1u);
        EXPECT_EQ(facts[0].object, "emacs");
        EXPECT_NEAR(facts[0].confidence, 0.95, 1e-9);
    }

    // matching upsert should reinforce confidence toward 1
    mc.upsertFact(mk("user", "favourite_editor", "emacs", 0.50));
    {
        auto facts = mc.queryFacts("user", "favourite_editor");
        ASSERT_EQ(facts.size(), 1u);
        EXPECT_EQ(facts[0].object, "emacs");
        EXPECT_GE(facts[0].confidence, 0.95);        // bumped, not lowered
    }
}

// 5 ─ Mood survives full cortex shutdown/reopen via mood.json.
TEST(MemoryCortexTest, MoodPersistsAcrossOpen) {
    TempRoot tmp("mood_persist");
    auto cfg = defaultCfg(tmp.path());

    {
        MemoryCortex mc(cfg);
        mc.nudgeMood(0.6, 0.5);                      // → "excited"
        auto m = mc.mood();
        EXPECT_GT(m.valence, 0.5);
        EXPECT_GT(m.arousal, 0.4);
        EXPECT_NE(m.current, "neutral");
    }

    {
        MemoryCortex mc(cfg);
        auto m = mc.mood();
        EXPECT_GT(m.valence, 0.5);
        EXPECT_GT(m.arousal, 0.4);
        EXPECT_NE(m.current, "neutral");
    }
}

// 6 ─ Mood decays exponentially toward zero: short half-life forces drift in 1s.
TEST(MemoryCortexTest, MoodDecaysTowardZero) {
    TempRoot tmp("mood_decay");
    auto cfg = defaultCfg(tmp.path());
    cfg.mood_half_life_seconds = 0.5;

    MemoryCortex mc(cfg);
    mc.nudgeMood(1.0, 0.0);
    auto before = mc.mood();
    EXPECT_GT(before.valence, 0.9);

    std::this_thread::sleep_for(milliseconds(1100));   // ≥ 2 half-lives
    mc.tickMoodDecay();

    auto after = mc.mood();
    EXPECT_LT(after.valence, 0.5);                    // decayed substantially
    EXPECT_LT(after.valence, before.valence);
}

// 7 ─ Recall: FTS-only path returns the right episodic event on top, with the
// rendered string carrying the mood tag for downstream prompt builders.
TEST(MemoryCortexTest, RecallTopsBestEpisodicHit) {
    TempRoot tmp("recall");
    MemoryCortex mc(defaultCfg(tmp.path()));

    EpisodicEvent e1;
    e1.kind     = "build_failure";
    e1.summary  = "cmake fails to link libfvad in arise build";
    e1.mood_at  = "frustrated";
    e1.salience = 0.8;
    int64_t id1 = mc.recordEvent(e1);
    ASSERT_GT(id1, 0);

    EpisodicEvent e2;
    e2.kind     = "casual";
    e2.summary  = "had pizza for dinner";
    e2.salience = 0.5;
    mc.recordEvent(e2);

    EpisodicEvent e3;
    e3.kind     = "casual";
    e3.summary  = "watched a movie tonight";
    e3.salience = 0.5;
    mc.recordEvent(e3);

    RecallQuery q;
    q.text  = "libfvad cmake link error";
    q.types = { MemoryType::Episodic };
    q.limit = 3;

    auto hits = mc.recall(q);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits[0].type, MemoryType::Episodic);
    ASSERT_TRUE(hits[0].episodic.has_value());
    EXPECT_EQ(hits[0].episodic->id, id1);
    EXPECT_NE(hits[0].rendered.find("frustrated"), std::string::npos);
    EXPECT_NE(hits[0].rendered.find("libfvad"),    std::string::npos);
}

// 8 ─ Identity store: record round-trips across reopen, mutations land as
// distinct git commits in the auto-managed repo.
TEST(IdentityStoreTest, RoundTripAndGitVersioning) {
    TempRoot tmp("identity");
    auto dir = tmp.path() + "/identity";

    {
        IdentityStore is(dir);
        IdentityRecord r = is.get();
        r.name             = "Test Pilot";
        r.persona_summary  = "Calm under pressure.";
        r.do_list          = { "ship daily" };
        r.dont_list        = { "yak-shave" };
        is.set(r, "init test pilot");
    }

    {
        IdentityStore is(dir);
        auto r = is.get();
        EXPECT_EQ(r.name, "Test Pilot");
        EXPECT_EQ(r.persona_summary, "Calm under pressure.");
        ASSERT_EQ(r.do_list.size(), 1u);
        EXPECT_EQ(r.do_list[0], "ship daily");
        ASSERT_EQ(r.dont_list.size(), 1u);
        EXPECT_EQ(r.dont_list[0], "yak-shave");

        r.name = "Test Pilot Mk II";
        is.set(r, "rename pilot");
    }

    // Confirm git history shows two commits (init test pilot + rename pilot).
    std::ostringstream cmd;
    cmd << "git -C \"" << dir << "\" log --oneline 2>/dev/null | wc -l";
    FILE* p = popen(cmd.str().c_str(), "r");
    ASSERT_NE(p, nullptr);
    char buf[16] = {};
    auto n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    ASSERT_GT(n, 0u);
    EXPECT_GE(std::atoi(buf), 2);
}
