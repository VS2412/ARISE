// Phase 5 commit 2: ForgeTool + sweepStale + sanitiseToolId.
//
// Deterministic tests cover id sanitisation, sweep semantics, the
// "unreachable LLM produces a clean error" path, ForgeTool's static
// helpers, and an end-to-end forge that auto-skips when bwrap or Ollama
// are missing.

#include "cortex/coder.hpp"
#include "cortex/critic.hpp"
#include "cortex/forge_tool.hpp"
#include "cortex/sandbox.hpp"
#include "cortex/sub_agent.hpp"
#include "cortex/tool_registry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

std::string mkSandbox(const std::string& tag) {
    auto base = fs::temp_directory_path()
              / ("arise_p5c2_test_" + tag + "_"
                 + std::to_string(::getpid()) + "_"
                 + std::to_string(duration_cast<microseconds>(
                       system_clock::now().time_since_epoch()).count()));
    fs::create_directories(base);
    return base.string();
}

void writeFile(const std::string& path, const std::string& content) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
    f.close();
}

bool haveBwrap() { return arise::Sandbox::isAvailable(); }

std::string isoDaysAgo(int days) {
    auto t = system_clock::to_time_t(
        system_clock::now() - hours(24 * days));
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof buf, "%FT%TZ", &tm_buf);
    return buf;
}

} // namespace

// ─── id sanitiser ──────────────────────────────────────────────────────────

TEST(ForgeIdSanitise, BasicCases) {
    auto root = mkSandbox("sanid");
    arise::ToolRegistry::Config c; c.root = root + "/tools";
    arise::ToolRegistry reg(c);

    EXPECT_EQ(reg.sanitiseToolId("count_todos"),    "count_todos");
    EXPECT_EQ(reg.sanitiseToolId("Count TODOs"),    "count_todos");
    EXPECT_EQ(reg.sanitiseToolId("  trim me  "),    "trim_me");
    EXPECT_EQ(reg.sanitiseToolId("my-cool tool!"),  "my_cool_tool");
    EXPECT_EQ(reg.sanitiseToolId("__a__b__"),       "a_b");
    EXPECT_NE(reg.sanitiseToolId(""),               "");
    EXPECT_NE(reg.sanitiseToolId("###"),            "");
}

TEST(ForgeIdSanitise, DedupesAgainstExisting) {
    auto root = mkSandbox("sanid_dup");
    arise::ToolRegistry::Config c; c.root = root + "/tools";
    arise::ToolRegistry reg(c);

    arise::ToolInfo t; t.id = "count_todos"; t.description = "x";
    EXPECT_TRUE(reg.addLearned(t));

    auto suggested = reg.sanitiseToolId("Count Todos");
    EXPECT_NE(suggested, "count_todos");
    EXPECT_EQ(suggested.rfind("count_todos", 0), 0u);
}

TEST(ForgeIdSanitise, RejectsBuiltinPrefix) {
    auto root = mkSandbox("sanid_builtin");
    arise::ToolRegistry::Config c; c.root = root + "/tools";
    arise::ToolRegistry reg(c);
    auto id = reg.sanitiseToolId("builtin:mem_record");
    // The "builtin:" prefix becomes "builtin_mem_record" after sanitising —
    // we just need to confirm the result doesn't start with "builtin:".
    EXPECT_NE(id.rfind("builtin:", 0), 0u);
}

// ─── sweepStale ─────────────────────────────────────────────────────────────

TEST(ToolSweep, ArchivesStaleNotFresh) {
    auto root = mkSandbox("sweep");
    arise::ToolRegistry::Config c; c.root = root + "/tools";
    arise::ToolRegistry reg(c);

    arise::ToolInfo old; old.id = "old_tool";
    old.last_used  = isoDaysAgo(120);
    old.created_at = isoDaysAgo(150);
    reg.addLearned(old);

    arise::ToolInfo fresh; fresh.id = "fresh_tool";
    fresh.last_used  = isoDaysAgo(2);
    fresh.created_at = isoDaysAgo(10);
    reg.addLearned(fresh);

    EXPECT_EQ(reg.sweepStale(90), 1);
    EXPECT_FALSE(reg.get("old_tool").has_value());
    EXPECT_TRUE (reg.get("fresh_tool").has_value());
    EXPECT_EQ(reg.listLearned().size(), 1u);
}

TEST(ToolSweep, DryRunDoesntMutate) {
    auto root = mkSandbox("sweep_dry");
    arise::ToolRegistry::Config c; c.root = root + "/tools";
    arise::ToolRegistry reg(c);

    arise::ToolInfo old; old.id = "old_tool";
    old.last_used  = isoDaysAgo(120);
    old.created_at = isoDaysAgo(150);
    reg.addLearned(old);

    EXPECT_EQ(reg.sweepStale(90, /*dry_run=*/true), 1);
    EXPECT_TRUE(reg.get("old_tool").has_value());
    EXPECT_EQ(reg.sweepStale(90, /*dry_run=*/false), 1);
    EXPECT_FALSE(reg.get("old_tool").has_value());
    EXPECT_TRUE(fs::exists(reg.archivedDir() + "/manifest.json"));
}

TEST(ToolSweep, NeverUsedAgesViaCreatedAt) {
    auto root = mkSandbox("sweep_unused");
    arise::ToolRegistry::Config c; c.root = root + "/tools";
    arise::ToolRegistry reg(c);

    arise::ToolInfo t; t.id = "stale_unused";
    t.created_at = isoDaysAgo(120);
    reg.addLearned(t);
    EXPECT_EQ(reg.sweepStale(90), 1);
    EXPECT_FALSE(reg.get("stale_unused").has_value());
}

TEST(ToolSweep, KeepsTimestampUnparseable) {
    auto root = mkSandbox("sweep_bad");
    arise::ToolRegistry::Config c; c.root = root + "/tools";
    arise::ToolRegistry reg(c);
    arise::ToolInfo t; t.id = "weird";
    t.last_used = "not-a-date"; t.created_at = "also-bad";
    reg.addLearned(t);
    EXPECT_EQ(reg.sweepStale(1), 0);
    EXPECT_TRUE(reg.get("weird").has_value());
}

// ─── ForgeTool static helpers ──────────────────────────────────────────────

TEST(ForgeHelpers, PickEntryPoint) {
    EXPECT_EQ(arise::ForgeTool::pickEntryPoint({"x.py","count_todos.sh"},
                                               "count_todos"),
              "count_todos.sh");
    EXPECT_EQ(arise::ForgeTool::pickEntryPoint({"helpers.py","count_todos.py"},
                                               "count_todos"),
              "count_todos.py");
    EXPECT_EQ(arise::ForgeTool::pickEntryPoint({"a.sh","b.py","c.py"}, "x"),
              "a.sh");
    EXPECT_EQ(arise::ForgeTool::pickEntryPoint({"only.py"}, "x"),
              "only.py");
    EXPECT_EQ(arise::ForgeTool::pickEntryPoint({"raw"}, "x"),
              "raw");
    EXPECT_EQ(arise::ForgeTool::pickEntryPoint({}, "x"), "");
}

TEST(ForgeHelpers, InterpreterFor) {
    EXPECT_EQ(arise::ForgeTool::interpreterFor("foo.sh"), "bash");
    EXPECT_EQ(arise::ForgeTool::interpreterFor("foo.py"), "python3");
    EXPECT_EQ(arise::ForgeTool::interpreterFor("foo"),    "");
}

// ─── Forge with unreachable LLM ─────────────────────────────────────────────

TEST(ForgeFailFast, MissingConfigReportsError) {
    arise::ForgeTool::Config fc;       // everything empty
    arise::ForgeTool forge(fc);
    auto p = forge.propose("doing nothing", nlohmann::json::object());
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(p.error.empty());
}

TEST(ForgeFailFast, UnreachableLlmProducesCleanError) {
    auto root = mkSandbox("forge_fail");
    arise::ToolRegistry::Config rc; rc.root = root + "/tools";
    arise::ToolRegistry reg(rc);

    arise::SubAgent::Config lc;
    lc.role        = "forge";
    lc.format_json = true;
    lc.ollama_url  = "http://127.0.0.1:1";  // unreachable
    lc.timeout_sec = 1;
    lc.connect_timeout_sec = 1;
    arise::SubAgent llm(lc);

    arise::Critic::Config cc;
    arise::Critic critic(cc);

    arise::ForgeTool::Config fc;
    fc.coder_llm    = &llm;
    fc.critic       = &critic;
    fc.registry     = &reg;
    fc.sandbox_root = reg.sandboxDir();
    fc.learned_root = reg.learnedDir();
    arise::ForgeTool forge(fc);

    auto p = forge.propose("count TODOs in a directory",
                           nlohmann::json::object());
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(p.error.empty());
    EXPECT_FALSE(p.id.empty());            // id was generated before LLM call
}

// ─── stage round-trip with a hand-crafted proposal ────────────────────────

TEST(ForgeStage, RoundTripFromHandcraftedProposal) {
    auto root = mkSandbox("forge_stage");
    arise::ToolRegistry::Config rc; rc.root = root + "/tools";
    arise::ToolRegistry reg(rc);

    // Hand-build a "proposal" that mimics Coder having succeeded.
    auto draft_dir = root + "/tools/sandbox/coder_handcrafted";
    fs::create_directories(draft_dir);
    auto entry = "stage_demo.sh";
    writeFile(draft_dir + "/" + entry,
              "#!/bin/sh\nread -r blob\necho got=$blob\n");

    arise::SubAgent::Config lc;          // unreachable; only used to construct
    arise::SubAgent llm(lc);
    arise::Critic::Config cc;
    arise::Critic critic(cc);

    arise::ForgeTool::Config fc;
    fc.coder_llm    = &llm;
    fc.critic       = &critic;
    fc.registry     = &reg;
    fc.sandbox_root = reg.sandboxDir();
    fc.learned_root = reg.learnedDir();
    arise::ForgeTool forge(fc);

    arise::ForgeTool::Proposal p;
    p.ok          = true;
    p.id          = reg.sanitiseToolId("stage demo");
    p.description = "echoes stdin";
    p.summary     = "demo";
    p.draft_dir   = draft_dir;
    p.entry_path  = draft_dir + "/" + entry;
    p.entry_rel   = entry;
    p.interpreter = "bash";
    p.files       = { p.entry_path };
    p.args_schema = nlohmann::json::object();

    EXPECT_TRUE(forge.stage(p, /*auto_approve=*/true));

    auto landed = reg.get(p.id);
    ASSERT_TRUE(landed.has_value());
    EXPECT_TRUE(landed->approved);
    EXPECT_EQ(landed->approved_by, "forge");
    EXPECT_EQ(landed->interpreter, "bash");
    EXPECT_TRUE(fs::exists(landed->script_path));
    EXPECT_FALSE(fs::exists(draft_dir));    // draft dir gets cleaned up
}

// ─── End-to-end forge (live qwen3:0.6b + bwrap) ────────────────────────────

TEST(ForgeEnd2End, ProducesAndStagesWorkingTool) {
    if (!haveBwrap()) GTEST_SKIP() << "bwrap missing; skipping end-to-end forge";

    auto root = mkSandbox("forge_e2e");
    arise::ToolRegistry::Config rc; rc.root = root + "/tools";
    arise::ToolRegistry reg(rc);

    arise::SubAgent::Config lc;
    lc.role        = "forge";
    lc.format_json = true;
    lc.timeout_sec = 90;
    lc.max_predict = 1500;
    arise::SubAgent llm(lc);
    if (!llm.run("ping").reachable) {
        GTEST_SKIP() << "ollama unreachable; skipping";
    }

    arise::Critic::Config cc;
    arise::Critic critic(cc);

    arise::ForgeTool::Config fc;
    fc.coder_llm     = &llm;
    fc.critic        = &critic;
    fc.registry      = &reg;
    fc.sandbox_root  = reg.sandboxDir();
    fc.learned_root  = reg.learnedDir();
    arise::ForgeTool forge(fc);

    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"text", {{"type","string"}}}}},
        {"required", {"text"}},
    };
    auto p = forge.propose(
        "Read JSON {\"text\": \"...\"} from stdin and print only the byte length of text.",
        schema, nlohmann::json{{"text","hello"}}, "byte_len");

    // Even if qwen3:0.6b's script output is rough, the pipeline shape should
    // be exercised: id was sanitised, draft dir was created, and either ok or
    // a clear error path was taken.
    EXPECT_FALSE(p.id.empty());
    EXPECT_FALSE(p.draft_dir.empty());
    if (!p.ok) {
        EXPECT_FALSE(p.error.empty()) << "non-ok proposal must explain itself";
    } else {
        EXPECT_TRUE(forge.stage(p, /*auto_approve=*/false));
        auto landed = reg.get(p.id);
        ASSERT_TRUE(landed.has_value());
        EXPECT_FALSE(landed->approved);   // not auto-approved
    }
}
