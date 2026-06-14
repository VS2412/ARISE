// Phase 3 commit 1: GoalStore — schema + lifecycle + queries + episodic mirror.
//
// Each test runs against a fresh sandbox under a unique tmpdir so they don't
// touch the developer's real ~/.arise. We exercise the full surface area
// the scheduler/resumer in commit 2 will lean on.

#include "cortex/goals.hpp"
#include "cortex/memory_cortex.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;

namespace {

std::string mkSandbox(const std::string& tag) {
    auto base = fs::temp_directory_path()
              / ("arise_goals_test_" + tag + "_"
                 + std::to_string(::getpid()) + "_"
                 + std::to_string(duration_cast<microseconds>(
                       system_clock::now().time_since_epoch()).count()));
    fs::create_directories(base);
    return base.string();
}

arise::GoalStore::Config sandboxCfg(const std::string& dir,
                                    arise::MemoryCortex* sink = nullptr) {
    arise::GoalStore::Config c;
    c.db_path       = dir + "/goals.db";
    c.episodic_sink = sink;
    return c;
}

arise::Goal makeProposal(std::string s, int prio = 50) {
    arise::Goal g;
    g.summary  = std::move(s);
    g.priority = prio;
    return g;
}

} // namespace

// ─── enum bridge & transitions ──────────────────────────────────────────────

TEST(GoalsEnum, RoundTripsAllStatuses) {
    using S = arise::GoalStatus;
    for (auto s : {S::Proposed, S::Accepted, S::InProgress, S::Blocked,
                   S::Done,     S::Rejected, S::Cancelled}) {
        auto back = arise::goalStatusFromString(arise::toString(s));
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, s);
    }
}

TEST(GoalsEnum, RejectsUnknownString) {
    EXPECT_FALSE(arise::goalStatusFromString("garbage").has_value());
    EXPECT_FALSE(arise::goalStatusFromString("").has_value());
}

TEST(GoalsTransitions, MatrixMatchesSpec) {
    using S = arise::GoalStatus;
    EXPECT_TRUE (arise::isValidTransition(S::Proposed,   S::Accepted));
    EXPECT_TRUE (arise::isValidTransition(S::Proposed,   S::Rejected));
    EXPECT_TRUE (arise::isValidTransition(S::Proposed,   S::Cancelled));
    EXPECT_FALSE(arise::isValidTransition(S::Proposed,   S::InProgress));
    EXPECT_FALSE(arise::isValidTransition(S::Proposed,   S::Done));

    EXPECT_TRUE (arise::isValidTransition(S::Accepted,   S::InProgress));
    EXPECT_TRUE (arise::isValidTransition(S::Accepted,   S::Blocked));
    EXPECT_TRUE (arise::isValidTransition(S::Accepted,   S::Done));
    EXPECT_FALSE(arise::isValidTransition(S::Accepted,   S::Rejected));

    EXPECT_TRUE (arise::isValidTransition(S::InProgress, S::Blocked));
    EXPECT_TRUE (arise::isValidTransition(S::InProgress, S::Done));
    EXPECT_FALSE(arise::isValidTransition(S::InProgress, S::Proposed));

    EXPECT_TRUE (arise::isValidTransition(S::Blocked,    S::InProgress));
    EXPECT_TRUE (arise::isValidTransition(S::Blocked,    S::Done));

    // Terminal states are sealed.
    for (auto term : {S::Done, S::Rejected, S::Cancelled}) {
        for (auto next : {S::Proposed, S::Accepted, S::InProgress, S::Blocked}) {
            EXPECT_FALSE(arise::isValidTransition(term, next))
                << "from=" << arise::toString(term) << " to=" << arise::toString(next);
        }
    }

    // Idempotent self-write is fine.
    EXPECT_TRUE(arise::isValidTransition(S::Done, S::Done));
}

// ─── propose + get ──────────────────────────────────────────────────────────

TEST(GoalsCrud, ProposeAndGetRoundTrip) {
    auto dir = mkSandbox("crud");
    arise::GoalStore store(sandboxCfg(dir));

    auto g = makeProposal("plan the cmake refactor", 70);
    g.tags = {"cmake", "refactor"};
    g.deadline_at = system_clock::now() + hours(48);

    auto id = store.propose(g);
    ASSERT_GT(id, 0);

    auto got = store.get(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->summary, "plan the cmake refactor");
    EXPECT_EQ(got->priority, 70);
    EXPECT_EQ(got->status, arise::GoalStatus::Proposed);
    ASSERT_EQ(got->tags.size(), 2u);
    EXPECT_EQ(got->tags[0], "cmake");
    ASSERT_TRUE(got->deadline_at.has_value());
    // created_at and last_progress_at start equal-ish.
    EXPECT_LE(std::abs(duration_cast<seconds>(
                  got->created_at - got->last_progress_at).count()),
              1);
}

TEST(GoalsCrud, RejectsEmptySummary) {
    auto dir = mkSandbox("empty");
    arise::GoalStore store(sandboxCfg(dir));
    auto id = store.propose(makeProposal("", 0));
    EXPECT_EQ(id, 0);
}

// ─── lifecycle enforcement ──────────────────────────────────────────────────

TEST(GoalsLifecycle, EnforcesValidPaths) {
    auto dir = mkSandbox("lifecycle");
    arise::GoalStore store(sandboxCfg(dir));

    auto id = store.propose(makeProposal("ship feature X"));
    EXPECT_TRUE (store.accept   (id));
    EXPECT_TRUE (store.start    (id));
    EXPECT_TRUE (store.block    (id, "waiting on libfvad fix"));
    {
        auto g = store.get(id);
        ASSERT_TRUE(g.has_value());
        EXPECT_EQ(g->status, arise::GoalStatus::Blocked);
        EXPECT_EQ(g->blocked_reason, "waiting on libfvad fix");
    }
    EXPECT_TRUE (store.unblock  (id, "fixed"));
    {
        auto g = store.get(id);
        ASSERT_TRUE(g.has_value());
        EXPECT_EQ(g->status, arise::GoalStatus::InProgress);
        EXPECT_EQ(g->blocked_reason, "");
    }
    EXPECT_TRUE (store.complete (id));
    EXPECT_FALSE(store.start    (id));   // terminal
    EXPECT_FALSE(store.cancel   (id));
}

TEST(GoalsLifecycle, BlockRefusesFromProposed) {
    auto dir = mkSandbox("blockfromprop");
    arise::GoalStore store(sandboxCfg(dir));
    auto id = store.propose(makeProposal("draft"));
    // Proposed → Blocked is not in the matrix.
    EXPECT_FALSE(store.block(id, "any"));
}

TEST(GoalsLifecycle, UnblockRefusesIfNotBlocked) {
    // Contract: unblock() is strict — only valid when current status is
    // Blocked. From Accepted, callers must `start()` instead.
    auto dir = mkSandbox("unblock");
    arise::GoalStore store(sandboxCfg(dir));
    auto id = store.propose(makeProposal("x"));
    EXPECT_TRUE (store.accept (id));
    EXPECT_FALSE(store.unblock(id));
    auto g = store.get(id);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->status, arise::GoalStatus::Accepted);  // unchanged
}

TEST(GoalsLifecycle, UnblockClearsReason) {
    auto dir = mkSandbox("unblock_reason");
    arise::GoalStore store(sandboxCfg(dir));
    auto id = store.propose(makeProposal("y"));
    EXPECT_TRUE(store.accept (id));
    EXPECT_TRUE(store.start  (id));
    EXPECT_TRUE(store.block  (id, "waiting on something"));
    EXPECT_TRUE(store.unblock(id));
    auto g = store.get(id);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->status, arise::GoalStatus::InProgress);
    EXPECT_EQ(g->blocked_reason, "");
}

// ─── FTS5 search ────────────────────────────────────────────────────────────

TEST(GoalsSearch, FindsBySummaryToken) {
    auto dir = mkSandbox("fts");
    arise::GoalStore store(sandboxCfg(dir));

    store.propose(makeProposal("rewrite the cmake build system"));
    store.propose(makeProposal("triage error reports"));
    auto id3 = store.propose(makeProposal("draft cmake refactor plan"));

    arise::GoalQuery q;
    q.text = "cmake refactor";
    q.limit = 10;
    auto rows = store.list(q);
    EXPECT_GE(rows.size(), 2u);
    bool seen3 = false;
    for (auto& g : rows) if (g.id == id3) seen3 = true;
    EXPECT_TRUE(seen3);
}

TEST(GoalsSearch, NoMatchYieldsEmpty) {
    auto dir = mkSandbox("fts_none");
    arise::GoalStore store(sandboxCfg(dir));
    store.propose(makeProposal("alpha"));
    arise::GoalQuery q; q.text = "absolutelynowhere";
    EXPECT_TRUE(store.list(q).empty());
}

TEST(GoalsSearch, UpdateReindexesFts) {
    auto dir = mkSandbox("fts_upd");
    arise::GoalStore store(sandboxCfg(dir));
    auto id = store.propose(makeProposal("xyz original"));
    EXPECT_TRUE(store.setSummary(id, "renamed completely different"));

    arise::GoalQuery q; q.text = "renamed";
    auto rows = store.list(q);
    EXPECT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].id, id);

    arise::GoalQuery q2; q2.text = "xyz";
    EXPECT_TRUE(store.list(q2).empty());
}

// ─── parent / child / subtree ───────────────────────────────────────────────

TEST(GoalsTree, ChildrenAndSubtreeBfs) {
    auto dir = mkSandbox("tree");
    arise::GoalStore store(sandboxCfg(dir));

    auto root = store.propose(makeProposal("ship 2026 cmake refactor"));
    arise::Goal sub1; sub1.summary = "fix CI"; sub1.parent_id = root;
    arise::Goal sub2; sub2.summary = "draft README"; sub2.parent_id = root;
    auto s1 = store.propose(sub1);
    auto s2 = store.propose(sub2);
    arise::Goal sub1a; sub1a.summary = "patch lint config"; sub1a.parent_id = s1;
    auto s1a = store.propose(sub1a);

    auto kids = store.children(root);
    EXPECT_EQ(kids.size(), 2u);

    auto tree = store.subtree(root);
    EXPECT_EQ(tree.size(), 4u);
    EXPECT_EQ(tree[0].id, root);
    // BFS depth-1 nodes appear before depth-2.
    bool seen_s1a_after_s1 = false;
    int idx_s1 = -1, idx_s1a = -1;
    for (size_t i = 0; i < tree.size(); ++i) {
        if (tree[i].id == s1)  idx_s1  = i;
        if (tree[i].id == s1a) idx_s1a = i;
    }
    if (idx_s1 >= 0 && idx_s1a >= 0) seen_s1a_after_s1 = idx_s1a > idx_s1;
    EXPECT_TRUE(seen_s1a_after_s1);

    auto chain = store.ancestors(s1a);
    EXPECT_EQ(chain.size(), 2u);
    EXPECT_EQ(chain.front().id, root);
    EXPECT_EQ(chain.back().id,  s1);
    (void)s2;
}

// ─── due / stale queries ────────────────────────────────────────────────────

TEST(GoalsScheduling, DueSoonRespectsHorizonAndTerminalFilter) {
    auto dir = mkSandbox("due");
    arise::GoalStore store(sandboxCfg(dir));

    arise::Goal a = makeProposal("imminent");
    a.deadline_at = system_clock::now() + minutes(5);
    auto a_id = store.propose(a);

    arise::Goal b = makeProposal("far away");
    b.deadline_at = system_clock::now() + hours(72);
    store.propose(b);

    arise::Goal c = makeProposal("already done, ignore me");
    c.deadline_at = system_clock::now() + minutes(1);
    auto c_id = store.propose(c);
    store.accept(c_id);
    store.complete(c_id);

    auto rows = store.dueSoon(hours(1));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].id, a_id);
}

TEST(GoalsScheduling, StaleInProgressFlagsByLastProgress) {
    auto dir = mkSandbox("stale");
    arise::GoalStore store(sandboxCfg(dir));

    auto id = store.propose(makeProposal("languishing task"));
    store.accept(id);
    store.start(id);

    // 10-day threshold — a fresh row is nowhere near stale.
    EXPECT_TRUE(store.staleInProgress(hours(24 * 10)).empty());

    // Wait past one second so the SQL `last_progress_at < now - threshold`
    // comparison crosses a tick. With second-resolution epochs we'd otherwise
    // race ourselves: created_at == now → no result with threshold=0.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    auto rows = store.staleInProgress(seconds(0));
    ASSERT_FALSE(rows.empty());        // assert, not expect — front() below
    EXPECT_EQ(rows.front().id, id);

    // bumpProgress refreshes last_progress_at; an hour-wide threshold should
    // now exclude it.
    EXPECT_TRUE(store.bumpProgress(id));
    EXPECT_TRUE(store.staleInProgress(hours(1)).empty());
}

// ─── plan / priority / deadline / tags / touch ──────────────────────────────

TEST(GoalsFieldUpdates, PlanPriorityDeadlineTags) {
    auto dir = mkSandbox("fields");
    arise::GoalStore store(sandboxCfg(dir));
    auto id = store.propose(makeProposal("update fields"));

    EXPECT_TRUE(store.setPlanJson(id, R"({"steps":[1,2,3]})"));
    EXPECT_TRUE(store.setPriority(id, 92));
    auto when = system_clock::now() + hours(36);
    EXPECT_TRUE(store.setDeadline(id, when));
    EXPECT_TRUE(store.setTags(id, {"a","b","c"}));

    auto g = store.get(id);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->plan_json, R"({"steps":[1,2,3]})");
    EXPECT_EQ(g->priority,  92);
    ASSERT_TRUE(g->deadline_at.has_value());
    EXPECT_EQ(g->tags.size(), 3u);
    EXPECT_EQ(g->tags[1], "b");

    // Clear deadline.
    EXPECT_TRUE(store.setDeadline(id, std::nullopt));
    auto g2 = store.get(id);
    EXPECT_FALSE(g2->deadline_at.has_value());
}

TEST(GoalsFieldUpdates, MissingIdReturnsFalse) {
    auto dir = mkSandbox("missing");
    arise::GoalStore store(sandboxCfg(dir));
    EXPECT_FALSE(store.setPlanJson(9999, "{}"));
    EXPECT_FALSE(store.setPriority(9999, 50));
    EXPECT_FALSE(store.bumpProgress(9999));
    EXPECT_FALSE(store.complete(9999));
}

// ─── tag filter on list ─────────────────────────────────────────────────────

TEST(GoalsList, TagFilter) {
    auto dir = mkSandbox("tags");
    arise::GoalStore store(sandboxCfg(dir));

    arise::Goal a = makeProposal("a"); a.tags = {"alpha","shared"};
    arise::Goal b = makeProposal("b"); b.tags = {"beta","shared"};
    arise::Goal c = makeProposal("c"); c.tags = {"gamma"};
    store.propose(a); store.propose(b); store.propose(c);

    arise::GoalQuery q; q.tag = "shared"; q.limit = 10;
    auto rows = store.list(q);
    EXPECT_EQ(rows.size(), 2u);

    arise::GoalQuery q2; q2.tag = "gamma";
    EXPECT_EQ(store.list(q2).size(), 1u);

    arise::GoalQuery q3; q3.tag = "noone";
    EXPECT_TRUE(store.list(q3).empty());
}

// ─── episodic mirror ────────────────────────────────────────────────────────

TEST(GoalsEpisodicMirror, WritesOnTransitions) {
    auto dir = mkSandbox("mirror");
    arise::MemoryCortex::Config mc;
    mc.root              = dir;
    mc.embed_cache_path  = dir + "/embed_cache.sqlite";
    mc.decay_thread      = false;
    arise::MemoryCortex cortex(mc);

    arise::GoalStore store(sandboxCfg(dir, &cortex));
    auto id = store.propose(makeProposal("audited cmake refactor"));
    EXPECT_TRUE(store.accept(id));
    EXPECT_TRUE(store.start (id));
    EXPECT_TRUE(store.block (id, "blocked on lint"));
    EXPECT_TRUE(store.unblock(id));
    EXPECT_TRUE(store.complete(id));

    auto recent = cortex.recentEvents(20);
    int kind_count = 0;
    for (const auto& ev : recent) if (ev.kind == "goal_state") ++kind_count;
    // propose + accept + start + block + unblock + complete = 6 mirrors.
    EXPECT_GE(kind_count, 5);
}

// ─── counts ─────────────────────────────────────────────────────────────────

TEST(GoalsCounts, ByStatusAndTotal) {
    auto dir = mkSandbox("counts");
    arise::GoalStore store(sandboxCfg(dir));

    auto a = store.propose(makeProposal("a"));
    auto b = store.propose(makeProposal("b"));
    store.propose(makeProposal("c"));

    store.accept(a);
    store.accept(b);
    store.start (a);
    store.complete(a);

    EXPECT_EQ(store.totalCount(), 3);
    EXPECT_EQ(store.countByStatus(arise::GoalStatus::Proposed),   1);
    EXPECT_EQ(store.countByStatus(arise::GoalStatus::Accepted),   1);
    EXPECT_EQ(store.countByStatus(arise::GoalStatus::Done),       1);
    EXPECT_EQ(store.countByStatus(arise::GoalStatus::InProgress), 0);
}
