// Phase 5 commit 1: Sandbox + ToolRegistry.
//
// Sandbox tests rely on a working `bwrap` binary on PATH. They auto-skip on
// hosts where it's missing. ToolRegistry tests are deterministic.

#include "cortex/sandbox.hpp"
#include "cortex/tool_registry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

std::string mkSandbox(const std::string& tag) {
    auto base = fs::temp_directory_path()
              / ("arise_p5c1_test_" + tag + "_"
                 + std::to_string(::getpid()) + "_"
                 + std::to_string(duration_cast<microseconds>(
                       system_clock::now().time_since_epoch()).count()));
    fs::create_directories(base);
    return base.string();
}

void writeScript(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
    f.close();
    chmod(path.c_str(), 0755);
}

bool haveBwrap() { return arise::Sandbox::isAvailable(); }

#define REQUIRE_BWRAP() do {                                       \
    if (!haveBwrap()) {                                            \
        GTEST_SKIP() << "bwrap unavailable; skipping isolation test"; \
    }                                                              \
} while (0)

} // namespace

// ─── Sandbox isolation ─────────────────────────────────────────────────────

TEST(SandboxBasic, MissingBwrapReportsErrorNotIsolation) {
    arise::Sandbox::Config c;
    c.bwrap_path = "definitely-not-a-real-binary-arise-test";
    arise::Sandbox s(c);
    auto r = s.run({"/bin/sh", "-c", "echo hello"});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("bwrap"), std::string::npos);
    EXPECT_NE(r.error.find("unavail"), std::string::npos);
}

TEST(SandboxBasic, EmptyArgvFails) {
    arise::Sandbox::Config c;
    arise::Sandbox s(c);
    auto r = s.run({});
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(SandboxBasic, KillBeforeSpawnIsImmediate) {
    arise::Sandbox::Config c;
    arise::Sandbox s(c);
    std::atomic<bool> kill{true};
    auto r = s.run({"/bin/sh", "-c", "true"}, "", &kill);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.killed);
}

TEST(SandboxIsolation, SimpleEcho) {
    REQUIRE_BWRAP();
    arise::Sandbox::Config c;
    arise::Sandbox s(c);
    auto r = s.run({"/bin/sh", "-c", "echo hello sandbox"});
    EXPECT_TRUE(r.ok) << "stderr=" << r.stderr_text << " err=" << r.error;
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(r.stdout_text.find("hello sandbox"), std::string::npos);
    EXPECT_FALSE(r.timed_out);
    EXPECT_GT(r.duration_ms, 0);
}

TEST(SandboxIsolation, TmpfsIsFresh) {
    REQUIRE_BWRAP();
    // Touch a file outside the sandbox; the sandbox /tmp must NOT see it.
    auto sentinel = mkSandbox("tmpcheck") + "/sentinel.txt";
    writeScript(sentinel, "outside-sandbox");

    arise::Sandbox::Config c;
    arise::Sandbox s(c);
    // /tmp inside sandbox is a fresh tmpfs — listing it should be empty.
    auto r = s.run({"/bin/sh", "-c",
                    "test -e /tmp/sentinel.txt && echo LEAKED; "
                    "ls -A /tmp | head -5"});
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.stdout_text.find("LEAKED"), std::string::npos)
        << "host /tmp leaked into sandbox";
}

TEST(SandboxIsolation, NetworkBlockedByDefault) {
    REQUIRE_BWRAP();
    arise::Sandbox::Config c;
    c.allow_network = false;
    c.timeout = 5s;
    arise::Sandbox s(c);
    // getent uses NSS / DNS; without --share-net the lookup either fails or
    // returns nothing. Accept any non-success exit OR empty stdout.
    auto r = s.run({"/bin/sh", "-c", "getent hosts example.com 2>&1"});
    // We only check that the sandbox didn't successfully resolve — i.e. the
    // command either failed, timed out, or printed nothing meaningful.
    EXPECT_TRUE(!r.ok || r.stdout_text.find("example.com") == std::string::npos);
}

TEST(SandboxIsolation, TimeoutKillsLongRunner) {
    REQUIRE_BWRAP();
    arise::Sandbox::Config c;
    c.timeout = 500ms;
    arise::Sandbox s(c);
    auto r = s.run({"/bin/sh", "-c", "sleep 5"});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.timed_out);
    EXPECT_LT(r.duration_ms, 3000);   // generous upper bound for slow hosts
}

TEST(SandboxIsolation, StdoutCapAndTruncate) {
    REQUIRE_BWRAP();
    arise::Sandbox::Config c;
    c.max_stdout_bytes = 256;
    arise::Sandbox s(c);
    auto r = s.run({"/bin/sh", "-c",
                    "for i in $(seq 1 100); do echo line_$i; done"});
    EXPECT_TRUE(r.ok);
    EXPECT_LE(r.stdout_text.size(), 256u);
    EXPECT_TRUE(r.stdout_truncated);
}

TEST(SandboxIsolation, ExitCodePropagated) {
    REQUIRE_BWRAP();
    arise::Sandbox::Config c;
    arise::Sandbox s(c);
    auto r = s.run({"/bin/sh", "-c", "exit 42"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.exit_code, 42);
    EXPECT_FALSE(r.timed_out);
}

TEST(SandboxIsolation, StdinIsForwarded) {
    REQUIRE_BWRAP();
    arise::Sandbox::Config c;
    arise::Sandbox s(c);
    auto r = s.run({"/bin/sh", "-c", "tr a-z A-Z"}, "hello world\n");
    EXPECT_TRUE(r.ok);
    EXPECT_NE(r.stdout_text.find("HELLO WORLD"), std::string::npos);
}

TEST(SandboxIsolation, EnvIsClearedByDefault) {
    REQUIRE_BWRAP();
    ::setenv("ARISE_SANDBOX_LEAK_TEST", "secret", 1);
    arise::Sandbox::Config c;
    arise::Sandbox s(c);
    auto r = s.run({"/bin/sh", "-c", "echo VAL=${ARISE_SANDBOX_LEAK_TEST:-missing}"});
    EXPECT_TRUE(r.ok);
    EXPECT_NE(r.stdout_text.find("VAL=missing"), std::string::npos);
    ::unsetenv("ARISE_SANDBOX_LEAK_TEST");
}

TEST(SandboxIsolation, ExplicitEnvIsForwarded) {
    REQUIRE_BWRAP();
    arise::Sandbox::Config c;
    c.env = { "ARISE_X=42" };
    arise::Sandbox s(c);
    auto r = s.run({"/bin/sh", "-c", "echo X=$ARISE_X"});
    EXPECT_TRUE(r.ok);
    EXPECT_NE(r.stdout_text.find("X=42"), std::string::npos);
}

TEST(SandboxIsolation, WritablePathBindMount) {
    REQUIRE_BWRAP();
    auto dir = mkSandbox("writable");
    arise::Sandbox::Config c;
    c.writable_paths = { dir };
    arise::Sandbox s(c);
    auto r = s.run({"/bin/sh", "-c",
                    std::string("touch \"" + dir + "/marker\" && echo ok")});
    EXPECT_TRUE(r.ok) << "stderr=" << r.stderr_text;
    EXPECT_TRUE(fs::exists(dir + "/marker"));
}

// ─── ToolRegistry round-trip + lifecycle ───────────────────────────────────

TEST(ToolRegistryBuiltins, CatalogIsNonEmpty) {
    auto& cat = arise::ToolRegistry::builtinTable();
    EXPECT_GE(cat.size(), 4u);
    bool has_mem  = false, has_goal = false, has_forge = false;
    for (auto& t : cat) {
        EXPECT_TRUE(t.is_builtin);
        EXPECT_TRUE(t.approved);
        EXPECT_EQ(t.id.rfind("builtin:", 0), 0u);
        if (t.id == "builtin:mem_record")    has_mem  = true;
        if (t.id == "builtin:goal_propose")  has_goal = true;
        if (t.id == "builtin:forge_tool")    has_forge = true;
    }
    EXPECT_TRUE(has_mem);
    EXPECT_TRUE(has_goal);
    EXPECT_TRUE(has_forge);
}

TEST(ToolRegistryRoundTrip, AddSaveReload) {
    auto root = mkSandbox("registry");
    arise::ToolRegistry::Config c; c.root = root + "/tools";
    {
        arise::ToolRegistry reg(c);
        arise::ToolInfo t;
        t.id = "count_lines";
        t.description = "count lines in a string";
        t.interpreter = "bash";
        t.script_path = root + "/tools/learned/count_lines.sh";
        t.args_schema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"text", {{"type","string"}}}}},
            {"required", {"text"}},
        };
        EXPECT_TRUE(reg.addLearned(t));
        EXPECT_EQ(reg.listLearned().size(), 1u);
    }

    arise::ToolRegistry reg2(c);
    auto rows = reg2.listLearned();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].id, "count_lines");
    EXPECT_FALSE(rows[0].approved);
    EXPECT_EQ(rows[0].interpreter, "bash");
}

TEST(ToolRegistryLifecycle, ApproveArchiveRemove) {
    auto root = mkSandbox("registry_lc");
    arise::ToolRegistry::Config c; c.root = root + "/tools";
    arise::ToolRegistry reg(c);

    arise::ToolInfo t; t.id = "demo"; t.description = "x";
    EXPECT_TRUE(reg.addLearned(t));

    EXPECT_FALSE(reg.get("demo")->approved);
    EXPECT_TRUE(reg.approve("demo", "user"));
    EXPECT_TRUE(reg.get("demo")->approved);
    EXPECT_EQ(reg.get("demo")->approved_by, "user");

    EXPECT_TRUE(reg.recordUse("demo"));
    EXPECT_EQ(reg.get("demo")->usage_count, 1);
    EXPECT_TRUE(reg.recordUse("demo"));
    EXPECT_EQ(reg.get("demo")->usage_count, 2);

    EXPECT_TRUE(reg.archive("demo"));
    EXPECT_FALSE(reg.get("demo").has_value());
    EXPECT_TRUE(fs::exists(reg.archivedDir() + "/manifest.json"));
}

TEST(ToolRegistryLifecycle, RejectsBuiltinIdInLearned) {
    auto root = mkSandbox("registry_builtin");
    arise::ToolRegistry::Config c; c.root = root + "/tools";
    arise::ToolRegistry reg(c);
    arise::ToolInfo t; t.id = "builtin:cant_overwrite";
    EXPECT_FALSE(reg.addLearned(t));
}

// ─── schema validation ─────────────────────────────────────────────────────

TEST(ToolRegistrySchema, AcceptsMatchingObject) {
    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {
            {"path",   {{"type","string"}}},
            {"limit",  {{"type","integer"}}},
            {"loud",   {{"type","boolean"}}},
        }},
        {"required", {"path"}},
    };
    EXPECT_EQ(arise::validateArgsAgainstSchema(schema,
                  {{"path","x"},{"limit",4},{"loud",true}}), "");
    EXPECT_EQ(arise::validateArgsAgainstSchema(schema, {{"path","x"}}), "");
}

TEST(ToolRegistrySchema, RejectsMissingRequired) {
    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"path", {{"type","string"}}}}},
        {"required", {"path"}},
    };
    auto err = arise::validateArgsAgainstSchema(schema, nlohmann::json::object());
    EXPECT_NE(err.find("required"), std::string::npos);
    EXPECT_NE(err.find("path"),     std::string::npos);
}

TEST(ToolRegistrySchema, RejectsTypeMismatch) {
    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"limit", {{"type","integer"}}}}},
    };
    auto err = arise::validateArgsAgainstSchema(schema, {{"limit","oops"}});
    EXPECT_NE(err.find("limit"),    std::string::npos);
    EXPECT_NE(err.find("integer"),  std::string::npos);
}

TEST(ToolRegistrySchema, EmptySchemaAcceptsAnything) {
    EXPECT_EQ(arise::validateArgsAgainstSchema(nlohmann::json::object(),
                                               {{"x",1}}), "");
}

// ─── end-to-end: register + run a learned tool through the registry+sandbox ─

TEST(SandboxedToolRun, RegisterApproveRunCounts) {
    REQUIRE_BWRAP();
    auto root = mkSandbox("e2e");
    arise::ToolRegistry::Config rc; rc.root = root + "/tools";
    arise::ToolRegistry reg(rc);

    auto script = reg.learnedDir() + "/echo.sh";
    writeScript(script, "#!/bin/sh\nread -r blob\necho got=$blob\n");

    arise::ToolInfo t;
    t.id          = "echo_blob";
    t.description = "echo whatever stdin says";
    t.interpreter = "bash";
    t.script_path = script;
    t.args_schema = nlohmann::json::object();
    EXPECT_TRUE(reg.addLearned(t));
    EXPECT_TRUE(reg.approve("echo_blob"));

    arise::Sandbox::Config sc;
    sc.timeout = 5s;
    sc.readonly_paths = { reg.learnedDir() };
    arise::Sandbox sb(sc);
    auto rr = sb.run({"bash", script}, "{\"hi\":1}");
    EXPECT_TRUE(rr.ok) << "stderr=" << rr.stderr_text;
    EXPECT_NE(rr.stdout_text.find("got={\"hi\":1}"), std::string::npos);

    EXPECT_TRUE(reg.recordUse("echo_blob"));
    EXPECT_EQ(reg.get("echo_blob")->usage_count, 1);
}
