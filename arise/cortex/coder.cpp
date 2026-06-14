#include "cortex/coder.hpp"

#include "cortex/sub_agent.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

constexpr const char* kSystemPrompt =
    "You are ARISE's Coder. Produce the files needed to satisfy the user's "
    "task. Reply with STRICT JSON ONLY:\n"
    "{\n"
    "  \"summary\": \"<one-sentence description>\",\n"
    "  \"files\": ["
        "{\"path\": \"<relative>\", \"content\": \"<full file body>\"}, …"
    "]\n"
    "}\n"
    "Rules:\n"
    "  * Paths are relative to the sandbox root, no leading '/' or '..'.\n"
    "  * Keep changes minimal. No installer scripts, no sudo, no rm -rf.\n"
    "  * Plain text only. Do not embed binaries.\n"
    "  * Pick safe shebangs (#!/usr/bin/env bash / python3) when scripting.";

std::string makeSandboxDir(const std::string& root) {
    std::error_code ec;
    fs::create_directories(root, ec);
    static thread_local std::mt19937_64 rng(
        std::random_device{}() ^ uint64_t(
            steady_clock::now().time_since_epoch().count()));
    auto suffix = std::to_string(rng() & 0xFFFFFFFFul);
    auto dir = fs::path(root) / ("coder_" + suffix);
    fs::create_directories(dir, ec);
    return dir.string();
}

// Reject paths with absolute or .. components, or empty.
bool relPathOk(const std::string& p) {
    if (p.empty()) return false;
    if (p.front() == '/') return false;
    if (p.find("..") != std::string::npos) return false;
    return true;
}

} // namespace

Coder::Coder(Config cfg) : cfg_(std::move(cfg)) {}

Coder::Result Coder::run(const std::string& task,
                         std::atomic<bool>* kill) {
    Result r;
    if (!cfg_.llm)    { r.error = "coder: llm not configured";    return r; }
    if (!cfg_.critic) { r.error = "coder: critic not configured"; return r; }
    if (cfg_.sandbox_root.empty()) {
        r.error = "coder: sandbox_root must be set"; return r;
    }
    if (task.empty()) { r.error = "coder: empty task"; return r; }

    if (kill && kill->load()) { r.killed = true; r.error = "killed"; return r; }
    auto deadline = steady_clock::now() + cfg_.max_wall_time;
    if (steady_clock::now() >= deadline) {
        r.budget_hit = true; r.error = "wall-time exceeded"; return r;
    }

    auto sandbox = makeSandboxDir(cfg_.sandbox_root);
    r.sandbox_path = sandbox;

    std::string prompt =
        std::string(kSystemPrompt) +
        "\n\nSANDBOX_ROOT: " + sandbox +
        "\n\nTASK:\n" + task +
        "\nReply with JSON only.";

    auto run = cfg_.llm->run(prompt);
    if (!run.ok) {
        r.error = "llm: " + (run.error.empty() ? "unparseable" : run.error);
        r.budget_hit = run.budget_hit;
        return r;
    }
    auto blob = SubAgent::firstJsonObject(run.output);
    if (!blob) { r.error = "coder: no JSON in llm output"; return r; }

    json j;
    try { j = json::parse(*blob); }
    catch (const std::exception& e) {
        r.error = std::string("coder: parse: ") + e.what();
        return r;
    }

    r.summary = j.value("summary", std::string{});
    if (!j.contains("files") || !j["files"].is_array()) {
        r.error = "coder: response missing files[]";
        return r;
    }

    std::string bundled;        // for Critic review
    int written = 0;
    for (auto& f : j["files"]) {
        if (kill && kill->load()) { r.killed = true; r.error = "killed"; return r; }
        if (!f.is_object()) continue;
        std::string rel     = f.value("path", std::string{});
        std::string content = f.value("content", std::string{});
        if (!relPathOk(rel)) {
            // Recovery: small models often echo back our SANDBOX_ROOT and
            // produce absolute paths under it. Strip the prefix and retry
            // the validator. Anything still failing (`..`, paths outside
            // the sandbox) gets the original rejection.
            std::string prefix = sandbox + "/";
            if (!rel.empty() && rel.front() == '/'
                && rel.rfind(prefix, 0) == 0) {
                rel = rel.substr(prefix.size());
            }
            if (!relPathOk(rel)) {
                r.error = "coder: rejected path '" + f.value("path", std::string{}) + "'";
                return r;
            }
        }
        if (int(content.size()) > cfg_.max_bytes_per_file) {
            r.error = "coder: file '" + rel + "' exceeds max_bytes_per_file";
            return r;
        }
        ++written;
        if (cfg_.max_files > 0 && written > cfg_.max_files) {
            r.error = "coder: too many files (cap=" +
                      std::to_string(cfg_.max_files) + ")";
            return r;
        }

        auto target = fs::path(sandbox) / rel;
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);

        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) { r.error = "coder: open failed: " + target.string(); return r; }
        out.write(content.data(), content.size());
        out.close();

        WrittenFile wf;
        wf.path     = target.string();
        wf.rel_path = rel;
        wf.bytes    = content.size();
        r.files.push_back(std::move(wf));

        bundled += "\n# === " + rel + " ===\n";
        bundled += content;
        bundled += "\n";
    }

    r.review = cfg_.critic->reviewContent(bundled, "coder bundle");
    r.ok = r.review.approved;
    if (!r.ok) r.error = "critic: " + r.review.verdict;
    return r;
}

} // namespace arise
