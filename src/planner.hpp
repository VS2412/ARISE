#pragma once

// Phase 11: Agentic Planner.
//
// Given a high-level goal ("reorganize my Downloads folder — PDFs to Documents,
// images to Pictures, delete anything over 30 days old"), the planner asks the
// LLM to produce a structured JSON plan, executes each step through the
// Executor, reflects on observations, optionally replans when a step fails,
// and confirms destructive steps with the user via a callback.
//
// Budget (hard caps, never overridden by the LLM):
//   max 10 steps, max 2 replans, 60s wall-clock from first step.

#include "llm.hpp"

#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class Executor;
class Memory;

enum class PlanStepStatus {
    Pending,
    Running,
    Done,
    Failed,
    Skipped,
    Denied,   // user refused at the confirmation gate
};

struct PlanStep {
    std::string tool;                                   // executor action type
    nlohmann::json args = nlohmann::json::object();
    std::string expected;                               // what we expect to observe
    std::string observation;                            // filled after execution
    PlanStepStatus status = PlanStepStatus::Pending;

    nlohmann::json toJson() const;
    static PlanStep fromJson(const nlohmann::json& j);
};

struct Plan {
    std::string goal;
    std::vector<PlanStep> steps;
    int dbId         = 0;   // row id in `plans` table (0 before first save)
    int usedReplans  = 0;
    std::chrono::steady_clock::time_point deadline;

    nlohmann::json toJson() const;
};

struct PlanResult {
    bool success     = false;
    bool aborted     = false;
    std::string summary;       // human-readable spoken outcome
    std::string abortReason;   // set when aborted
    int stepsExecuted = 0;
};

// Invoked for destructive steps. Return true to proceed, false to skip/abort.
// `prompt` is the voice-friendly question the daemon should speak.
using ConfirmCallback = std::function<bool(const std::string& prompt,
                                            const PlanStep& step)>;

// Optional progress notification — spoken inline so the user knows the planner
// is still working.
using ProgressCallback = std::function<void(const std::string& message)>;

// Polled between every step (and during long inner work). Return true to
// abort the plan immediately. The daemon implements this by draining the
// speech queue and matching interrupt phrases like "stop" / "cancel".
using AbortCheckCallback = std::function<bool()>;

// Build a short, voice-friendly description of what a step DOES — used for
// the once-per-plan confirmation prompt and the progress callback. NEVER
// includes raw JSON or shell strings; Piper would voice them literally.
std::string describeStep(const PlanStep& step);

class Planner {
public:
    Planner(LLM* llm, Executor* executor, Memory* memory);

    // Hard budget — enforced regardless of LLM output.
    static constexpr int kMaxSteps    = 10;
    static constexpr int kMaxReplans  = 2;
    static constexpr int kWallTimeSec = 60;

    // Ask the LLM for a plan. Returns a Plan with empty steps on failure so
    // callers can degrade gracefully to conversational fallback.
    Plan createPlan(const std::string& goal, const LLMContext& ctx);

    // Run a full plan end-to-end: execute → reflect → (replan | next | abort)
    // until done, aborted, or budget exhausted. `onConfirm` may be nullptr —
    // in that case destructive steps are refused for safety. `shouldAbort`
    // is polled between every step; if it returns true the plan exits early
    // with PlanResult.aborted == true.
    PlanResult run(const std::string& goal,
                   const LLMContext& ctx,
                   ConfirmCallback   onConfirm,
                   ProgressCallback  onProgress,
                   AbortCheckCallback shouldAbort = nullptr);

    // Exposed so callers (tests, daemon) can classify a step without a full
    // Planner instance.
    static bool isDestructive(const PlanStep& step);

private:
    LLM*      llm_      = nullptr;
    Executor* executor_ = nullptr;
    Memory*   memory_   = nullptr;

    std::string executeStep(PlanStep& step);

    // Reflection decision codes — keep as strings so the LLM can emit them.
    //   "continue"      — proceed to the next pending step
    //   "replan"        — regenerate the remaining steps
    //   "abort:<text>"  — stop immediately, speak <text>
    //   "done"          — plan's goal is satisfied, stop cleanly
    std::string reflect(const Plan& plan, size_t stepIdx, const LLMContext& ctx);

    // Regenerate the tail of the plan starting at `fromIdx`. Existing step
    // observations are provided to the LLM so it can course-correct.
    bool replan(Plan& plan, size_t fromIdx, const LLMContext& ctx);

    // Planning prompts (system-role strings).
    static std::string planSystemPrompt();
    static std::string reflectSystemPrompt();

    // Lists the executor actions the LLM is allowed to emit, with a one-line
    // description. Kept in a static table in planner.cpp.
    static std::string toolCatalogueText();

    // Stringify a plan for prompt injection (compact, one step per line).
    static std::string planToPromptText(const Plan& plan);

    // Find the "tool_calls"-free plan JSON in whatever the LLM returned. The
    // format=json setting means we almost always get a clean object, but
    // handle the odd prose wrapper defensively.
    static nlohmann::json extractPlanArray(const nlohmann::json& raw);
};
