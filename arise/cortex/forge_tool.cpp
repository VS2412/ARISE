#include "cortex/forge_tool.hpp"

#include "cortex/coder.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

std::string nowIso() {
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof buf, "%FT%TZ", &tm_buf);
    return buf;
}

std::string fileExt(const std::string& p) {
    auto dot = p.rfind('.');
    if (dot == std::string::npos) return {};
    return p.substr(dot);
}

std::string composeForgeTask(const std::string& id,
                             const std::string& description,
                             const json& args_schema) {
    std::ostringstream os;
    os << "Build a tool named '" << id << "'.\n"
       << "WHAT IT DOES: " << description << "\n"
       << "INVOCATION: arguments arrive as one JSON object on stdin, matching:\n"
       << args_schema.dump(2) << "\n"
       << "REQUIREMENTS:\n"
       << "  * Choose ONE entry-point script. Name it '" << id
       << ".sh' (bash) or '" << id << ".py' (python3).\n"
       << "  * The entry-point must read its args by reading stdin once into "
          "a JSON-decoded variable.\n"
       << "  * Exit 0 on success. Print only what the user actually wants — "
          "no banners, no debug spam.\n"
       << "  * No sudo. No network calls. No filesystem writes outside /tmp.\n"
       << "  * Plain text only. No third-party pip/npm dependencies.\n"
       << "Reply STRICTLY with the Coder JSON contract: "
          "{\"summary\": \"...\", \"files\": [{\"path\":\"...\",\"content\":\"...\"}]}.";
    return os.str();
}

} // namespace

std::string ForgeTool::pickEntryPoint(const std::vector<std::string>& rel,
                                      const std::string& id) {
    if (rel.empty()) return {};

    auto pref_sh = id + ".sh";
    auto pref_py = id + ".py";
    for (const auto& p : rel) if (p == pref_sh) return p;
    for (const auto& p : rel) if (p == pref_py) return p;

    std::vector<std::string> sh, py;
    for (const auto& p : rel) {
        auto e = fileExt(p);
        if (e == ".sh") sh.push_back(p);
        else if (e == ".py") py.push_back(p);
    }
    std::sort(sh.begin(), sh.end());
    std::sort(py.begin(), py.end());
    if (!sh.empty()) return sh.front();
    if (!py.empty()) return py.front();
    return rel.front();
}

std::string ForgeTool::interpreterFor(const std::string& path) {
    auto ext = fileExt(path);
    if (ext == ".sh") return "bash";
    if (ext == ".py") return "python3";
    return {};
}

ForgeTool::ForgeTool(Config cfg) : cfg_(std::move(cfg)) {}

ForgeTool::Proposal ForgeTool::propose(const std::string& description,
                                       const json& args_schema,
                                       std::optional<json> example_args,
                                       const std::string& requested_id) {
    Proposal r;
    r.description = description;
    r.args_schema = args_schema;

    if (!cfg_.coder_llm || !cfg_.critic || !cfg_.registry) {
        r.error = "forge: coder_llm / critic / registry are all required";
        return r;
    }
    if (cfg_.sandbox_root.empty()) {
        r.error = "forge: sandbox_root must be set";
        return r;
    }
    if (description.empty()) {
        r.error = "forge: empty description";
        return r;
    }

    // Sanitise / dedupe the id.
    auto id_seed = requested_id.empty()
        ? description.substr(0, 32)
        : requested_id;
    r.id = cfg_.registry->sanitiseToolId(id_seed);

    // Drive Coder.
    Coder::Config cc;
    cc.llm                  = cfg_.coder_llm;
    cc.critic               = cfg_.critic;
    cc.sandbox_root         = cfg_.sandbox_root;
    cc.max_files            = int(cfg_.coder_max_files);
    cc.max_bytes_per_file   = int(cfg_.coder_max_bytes_per_file);
    cc.max_wall_time        = cfg_.coder_max_wall_time;
    Coder coder(cc);

    auto coder_task = composeForgeTask(r.id, description, args_schema);
    auto cresult = coder.run(coder_task);
    r.summary = cresult.summary;
    r.draft_dir = cresult.sandbox_path;
    for (const auto& f : cresult.files) r.files.push_back(f.path);
    r.critic_review = cresult.review;
    if (cresult.budget_hit) { r.budget_hit = true; }

    if (!cresult.ok) {
        r.error = cresult.error.empty()
            ? "forge: coder did not produce an approved bundle"
            : "forge: " + cresult.error;
        return r;
    }
    if (cresult.files.empty()) {
        r.error = "forge: coder returned zero files";
        return r;
    }

    // Pick entry point.
    std::vector<std::string> rels;
    for (auto& f : cresult.files) rels.push_back(f.rel_path);
    auto entry_rel = pickEntryPoint(rels, r.id);
    if (entry_rel.empty()) {
        r.error = "forge: no entry-point script in bundle";
        return r;
    }
    r.entry_rel  = entry_rel;
    r.entry_path = (fs::path(r.draft_dir) / entry_rel).string();
    r.interpreter = interpreterFor(entry_rel);

    // Optional dry-run.
    if (!example_args.has_value()) {
        r.dry_run_skipped = true;
        r.ok = true;
        return r;
    }

    Sandbox::Config sc;
    sc.timeout            = std::chrono::milliseconds(cfg_.dry_run_timeout_sec * 1000);
    sc.max_stdout_bytes   = cfg_.dry_run_max_stdout;
    sc.max_stderr_bytes   = cfg_.dry_run_max_stderr;
    sc.readonly_paths     = { r.draft_dir };
    Sandbox sandbox(sc);

    std::vector<std::string> argv;
    if (!r.interpreter.empty()) argv.push_back(r.interpreter);
    argv.push_back(r.entry_path);
    r.dry_run = sandbox.run(argv, example_args->dump());

    r.ok = r.dry_run.ok;
    if (!r.ok) {
        if (!r.dry_run.error.empty()) {
            r.error = "forge: dry-run failed: " + r.dry_run.error;
        } else if (r.dry_run.timed_out) {
            r.error = "forge: dry-run timed out";
        } else {
            r.error = "forge: dry-run exited with code "
                    + std::to_string(r.dry_run.exit_code);
        }
    }
    return r;
}

bool ForgeTool::stage(const Proposal& proposal, bool auto_approve) {
    if (!proposal.ok) return false;
    if (!cfg_.registry) return false;
    if (proposal.id.empty() || proposal.draft_dir.empty()
        || proposal.entry_path.empty()) return false;
    if (cfg_.learned_root.empty()) return false;

    // Move (copy + remove on success) the bundle from sandbox to learned/<id>/.
    fs::path target = fs::path(cfg_.learned_root) / proposal.id;
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) return false;

    // Copy every file from the draft tree into target.
    for (const auto& abs : proposal.files) {
        fs::path src = abs;
        fs::path rel = fs::relative(src, proposal.draft_dir, ec);
        if (ec) return false;
        fs::path dst = target / rel;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
    }

    // Best-effort cleanup of the staging dir.
    fs::remove_all(proposal.draft_dir, ec);
    ec.clear();

    ToolInfo t;
    t.id              = proposal.id;
    t.version         = 1;
    t.description     = proposal.description;
    t.interpreter     = proposal.interpreter;
    t.script_path     = (target / proposal.entry_rel).string();
    t.args_schema     = proposal.args_schema;
    t.allow_network   = false;
    t.created_at      = nowIso();
    if (!cfg_.registry->addLearned(t)) return false;
    if (auto_approve) cfg_.registry->approve(t.id, "forge");
    return true;
}

} // namespace arise
