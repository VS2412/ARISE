#include "cortex/researcher.hpp"

#include "cortex/sub_agent.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

constexpr const char* kSystemPrompt =
    "You are ARISE's Researcher. Solve the user's task step by step using "
    "the tools below. Reply with STRICT JSON ONLY each turn:\n"
    "  {\"thought\": \"<one sentence>\","
    "   \"action\": \"<read_file|list_dir|final>\","
    "   \"args\": {<args>}}\n"
    "Tool surfaces:\n"
    "  read_file: {\"path\": \"<relative path under sandbox>\"}\n"
    "  list_dir:  {\"path\": \"<relative path or empty>\"}\n"
    "  final:     {\"answer\": \"<your final answer>\"}\n"
    "Rules:\n"
    "  * Paths are RELATIVE to the sandbox root. Reject yours if it escapes.\n"
    "  * Pick `final` as soon as you can answer — don't read just to read.\n"
    "  * Keep thoughts short. No prose outside JSON.";

std::string buildHistoryBlock(const std::vector<Researcher::Step>& steps) {
    std::ostringstream os;
    for (size_t i = 0; i < steps.size(); ++i) {
        os << "STEP " << (i+1) << "\n"
           << "  thought: " << steps[i].thought << "\n"
           << "  action:  " << steps[i].action << "\n"
           << "  args:    " << steps[i].args.dump() << "\n"
           << "  observation: " << steps[i].observation << "\n";
    }
    return os.str();
}

} // namespace

Researcher::Researcher(Config cfg) : cfg_(std::move(cfg)) {}

std::string Researcher::resolveSandboxPath_(const std::string& rel_path) const {
    if (cfg_.sandbox_root.empty()) return {};
    fs::path root_abs;
    std::error_code ec;
    root_abs = fs::weakly_canonical(cfg_.sandbox_root, ec);
    if (ec) root_abs = cfg_.sandbox_root;

    fs::path joined = (rel_path.empty() || rel_path == ".")
        ? root_abs
        : root_abs / rel_path;

    fs::path canon = fs::weakly_canonical(joined, ec);
    if (ec) canon = joined;

    // The canonical path must start with the sandbox root.
    auto root_str  = root_abs.string();
    auto canon_str = canon.string();
    if (canon_str.rfind(root_str, 0) != 0) return {};
    return canon_str;
}

std::string Researcher::runReadFile_(const std::string& rel_path) const {
    auto path = resolveSandboxPath_(rel_path);
    if (path.empty()) return "ERROR: path outside sandbox or empty";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return "ERROR: not a regular file";
    std::ifstream f(path, std::ios::binary);
    if (!f) return "ERROR: open failed";
    std::string buf;
    buf.resize(cfg_.max_file_bytes);
    f.read(buf.data(), buf.size());
    auto got = f.gcount();
    buf.resize(got);
    bool truncated = false;
    if (f.peek() != EOF) truncated = true;
    if (truncated) buf += "\n[…file truncated…]";
    return buf;
}

std::string Researcher::runListDir_(const std::string& rel_path) const {
    auto path = resolveSandboxPath_(rel_path);
    if (path.empty()) return "ERROR: path outside sandbox or empty";
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return "ERROR: not a directory";
    std::ostringstream os;
    int n = 0;
    for (auto& entry : fs::directory_iterator(path, ec)) {
        if (ec) break;
        os << entry.path().filename().string()
           << (fs::is_directory(entry.path(), ec) ? "/" : "")
           << "\n";
        if (++n >= cfg_.max_listing) { os << "[…truncated…]\n"; break; }
    }
    return os.str();
}

Researcher::Result Researcher::run(const std::string& task,
                                   std::atomic<bool>* kill) {
    Result r;
    // Kill check first: callers expect a pre-set kill flag to always trump
    // config errors so wrappers (Spawn handle) get a deterministic shutdown.
    if (kill && kill->load()) { r.killed = true; r.error = "killed"; return r; }
    if (!cfg_.llm) {
        r.error = "researcher: llm not configured";
        return r;
    }
    if (task.empty()) {
        r.error = "researcher: empty task";
        return r;
    }

    auto deadline = steady_clock::now() + cfg_.max_wall_time;
    int rounds = 0;
    while (true) {
        if (kill && kill->load()) { r.killed = true; r.error = "killed"; return r; }
        if (steady_clock::now() >= deadline) { r.budget_hit = true; r.error = "wall-time exceeded"; return r; }
        if (rounds >= cfg_.max_tool_calls) { r.budget_hit = true; r.error = "tool-call cap reached"; return r; }
        ++rounds;

        std::string history = buildHistoryBlock(r.steps);
        std::string prompt =
            std::string(kSystemPrompt) +
            "\n\nSANDBOX ROOT: " + cfg_.sandbox_root +
            "\n\nTASK:\n" + task +
            "\n\n" + (history.empty() ? std::string{}
                                       : "HISTORY:\n" + history + "\n") +
            "Reply with the next JSON action.";

        auto run = cfg_.llm->run(prompt);
        if (!run.ok) {
            r.error = "llm: " + (run.error.empty() ? "unparseable" : run.error);
            r.budget_hit = run.budget_hit;
            return r;
        }
        auto blob = SubAgent::firstJsonObject(run.output);
        if (!blob) {
            r.error = "researcher: no JSON in llm output";
            return r;
        }
        json j;
        try { j = json::parse(*blob); }
        catch (const std::exception& e) {
            r.error = std::string("researcher: parse: ") + e.what();
            return r;
        }

        Step step;
        step.thought = j.value("thought", std::string{});
        step.action  = j.value("action",  std::string{});
        step.args    = j.value("args",    json::object());

        if (step.action == "final") {
            step.observation = "(final)";
            r.steps.push_back(std::move(step));
            r.answer = r.steps.back().args.value("answer", std::string{});
            r.ok = !r.answer.empty();
            if (!r.ok) r.error = "final action without answer";
            return r;
        }
        if (step.action == "read_file") {
            step.observation = runReadFile_(step.args.value("path", std::string{}));
        } else if (step.action == "list_dir") {
            step.observation = runListDir_(step.args.value("path", std::string{}));
        } else {
            step.observation = "ERROR: unknown action '" + step.action + "'";
        }
        r.steps.push_back(std::move(step));
    }
}

} // namespace arise
