#include "planner.hpp"

#include "executor.hpp"
#include "logger.hpp"
#include "memory.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

using json = nlohmann::json;

// ─── PlanStep / Plan (de)serialization ───

json PlanStep::toJson() const {
    const char* statusStr = "pending";
    switch (status) {
        case PlanStepStatus::Pending:          statusStr = "pending";  break;
        case PlanStepStatus::Running:          statusStr = "running";  break;
        case PlanStepStatus::Done:             statusStr = "done";     break;
        case PlanStepStatus::Failed:           statusStr = "failed";   break;
        case PlanStepStatus::Skipped:          statusStr = "skipped";  break;
        case PlanStepStatus::Denied:           statusStr = "denied";   break;
    }
    return {
        {"tool",        tool},
        {"args",        args},
        {"expected",    expected},
        {"observation", observation},
        {"status",      statusStr},
    };
}

PlanStep PlanStep::fromJson(const json& j) {
    PlanStep s;
    if (j.contains("tool")     && j["tool"].is_string())     s.tool     = j["tool"].get<std::string>();
    if (j.contains("expected") && j["expected"].is_string()) s.expected = j["expected"].get<std::string>();
    if (j.contains("args")     && j["args"].is_object())     s.args     = j["args"];
    else                                                      s.args     = json::object();
    return s;
}

json Plan::toJson() const {
    json out;
    out["goal"] = goal;
    out["steps"] = json::array();
    for (const auto& st : steps) out["steps"].push_back(st.toJson());
    return out;
}

// ─── describeStep — voice-friendly one-line summary, no raw JSON ───
//
// Free-standing so the daemon can humanize confirmation prompts without
// touching Planner internals. Every branch must produce something Piper
// can voice naturally — no paths longer than a basename, no glob symbols,
// no JSON, no shell escapes.
std::string describeStep(const PlanStep& step) {
    using nlohmann::json;
    auto strArg = [&](const char* k) -> std::string {
        if (step.args.contains(k) && step.args[k].is_string())
            return step.args[k].get<std::string>();
        return "";
    };
    auto basename = [](const std::string& p) -> std::string {
        auto pos = p.find_last_of('/');
        return (pos == std::string::npos) ? p : p.substr(pos + 1);
    };

    const std::string& t = step.tool;

    if (t == "run") {
        std::string cmd = strArg("command");
        std::string lower; lower.reserve(cmd.size());
        for (char c : cmd) lower.push_back(static_cast<char>(std::tolower(c)));

        if (lower.find("mkdir") != std::string::npos)        return "create folders";
        if (lower.find("rm ")   != std::string::npos)        return "delete files";
        if (lower.find("mv ")   != std::string::npos ||
            lower.find("mv -t") != std::string::npos) {
            // Try to name the file type and destination.
            std::string types;
            if (lower.find(".pdf") != std::string::npos)     types = "PDFs";
            else if (lower.find(".jpg") != std::string::npos ||
                     lower.find(".png") != std::string::npos ||
                     lower.find(".jpeg") != std::string::npos) types = "images";
            else if (lower.find(".doc") != std::string::npos ||
                     lower.find(".ppt") != std::string::npos ||
                     lower.find(".xls") != std::string::npos) types = "documents";
            else types = "matching files";
            return "move " + types + " into a folder";
        }
        if (lower.find("cp ")   != std::string::npos)        return "copy files";
        if (lower.find("find")  != std::string::npos)        return "search the filesystem";
        return "run a shell command";
    }
    if (t == "open") {
        std::string app = strArg("app");
        return app.empty() ? "open an app" : "open " + app;
    }
    if (t == "url") {
        std::string url = strArg("url");
        return url.empty() ? "open a link" : "open a web link";
    }
    if (t == "type")           return "type some text";
    if (t == "volume")         return "change the volume";
    if (t == "brightness")     return "change the brightness";
    if (t == "media")          return "control media playback";
    if (t == "screenshot")     return "take a screenshot";
    if (t == "workspace")      return "switch workspace";
    if (t == "close")          return "close the focused window";
    if (t == "clipboard")      return "use the clipboard";
    if (t == "notify")         return "show a notification";
    if (t == "file_read") {
        return "read " + (basename(strArg("path")).empty() ? "a file" : basename(strArg("path")));
    }
    if (t == "file_write") {
        std::string b = basename(strArg("path"));
        return b.empty() ? "write a file" : "write " + b;
    }
    if (t == "file_search")    return "search for files";
    if (t == "file_list") {
        std::string b = basename(strArg("path"));
        return b.empty() ? "list a folder" : "list " + b;
    }
    if (t == "window")         return "rearrange a window";
    if (t == "list_windows")   return "list open windows";
    if (t == "focus_window")   return "focus a window";
    if (t == "proc_list")      return "list running processes";
    if (t == "proc_kill") {
        std::string tgt = strArg("target");
        return tgt.empty() ? "kill a process" : "kill " + tgt;
    }
    if (t == "timer_set")      return "start a timer";
    if (t == "timer_list")     return "list timers";
    if (t == "timer_cancel")   return "cancel a timer";
    if (t == "web_search")     return "search the web";
    if (t == "sysinfo")        return "check system info";
    if (t == "recall_memory")  return "search memory";
    if (t == "remember")       return "save a fact";
    if (t == "see_screen")     return "look at the screen";
    if (t == "read_screen_text") return "read text on the screen";

    return "do " + t;
}

// ─── Static helpers ───

// Keep this list tight and executor-aligned. Every tool name here must map to
// an Executor action (see executor.cpp::execute and daemon.cpp::executeAction).
// Update in lock-step when new executor actions ship.
std::string Planner::toolCatalogueText() {
    return
        "open               — open a desktop application. args: {\"app\": \"firefox\"}\n"
        "run                — execute a shell command (pacman only, no apt, no sudo rm without confirmation). args: {\"command\": \"ls ~/Downloads\"}\n"
        "url                — open a URL in the browser. args: {\"url\": \"https://...\"}\n"
        "type               — type text into the focused window. args: {\"text\": \"...\"}\n"
        "volume             — adjust volume. args: {\"action\": \"up|down|mute\"}\n"
        "brightness         — adjust brightness. args: {\"action\": \"up|down\"}\n"
        "media              — media keys. args: {\"action\": \"play|pause|next|prev|stop\"}\n"
        "screenshot         — take a screenshot. args: {}\n"
        "workspace          — switch workspace. args: {\"number\": \"2\"}\n"
        "close              — close focused window. args: {}\n"
        "clipboard          — read/write clipboard. args: {\"action\": \"read|write\", \"text\": \"...\"}\n"
        "notify             — desktop notification. args: {\"message\": \"...\"}\n"
        "file_read          — read a file. args: {\"path\": \"~/foo.txt\", \"max_lines\": 50}\n"
        "file_write         — write a file (DESTRUCTIVE). args: {\"path\": \"...\", \"content\": \"...\", \"append\": false}\n"
        "file_search        — find files by name. args: {\"directory\": \"~\", \"pattern\": \"*.pdf\", \"content_grep\": \"...\"}\n"
        "file_list          — list a directory. args: {\"path\": \"~\"}\n"
        "window             — window action. args: {\"action\": \"maximize|fullscreen|center|move-column-left|...\"}\n"
        "list_windows       — list open windows. args: {}\n"
        "focus_window       — focus by app name. args: {\"app\": \"code\"}\n"
        "proc_list          — list processes. args: {\"filter\": \"firefox\"}\n"
        "proc_kill          — kill a process (DESTRUCTIVE). args: {\"target\": \"pid or name\"}\n"
        "timer_set          — start a timer. args: {\"duration_seconds\": 60, \"label\": \"tea\"}\n"
        "timer_list         — list timers. args: {}\n"
        "timer_cancel       — cancel a timer. args: {\"id\": 1}\n"
        "web_search         — search the web. args: {\"query\": \"...\"}\n"
        "sysinfo            — system info. args: {\"category\": \"disk|memory|cpu|battery|network|all\"}\n"
        "recall_memory      — search long-term memory. args: {\"query\": \"...\"}\n"
        "remember           — persist a fact about the user. args: {\"key\": \"...\", \"value\": \"...\"}\n"
        "see_screen         — ask the VLM a question about the screen. args: {\"question\": \"...\"}\n"
        "read_screen_text   — OCR current screen. args: {}\n";
}

std::string Planner::planSystemPrompt() {
    std::ostringstream s;
    s << "You are ARIA's planner. Break a user goal into an ordered JSON plan of at most "
      << Planner::kMaxSteps
      << " executor steps.\n\n"
         "OUTPUT FORMAT — strict JSON, no prose, no markdown fences:\n"
         "{\n"
         "  \"steps\": [\n"
         "    {\"tool\": \"<name>\", \"args\": {...}, \"expected\": \"<what you'll observe>\"}\n"
         "  ]\n"
         "}\n\n"
         "RULES:\n"
         "- Each \"tool\" MUST be one of the exact action names listed below.\n"
         "- \"args\" MUST be a valid JSON object matching the tool's schema (even if empty: {}).\n"
         "- \"expected\" is ONE short sentence describing the observation you anticipate — used for reflection.\n"
         "- The plan MUST actually ACHIEVE the goal. If the goal asks you to CHANGE state (move/organize/delete/create/rename/install/write),\n"
         "  the plan MUST contain the steps that DO the change — not just steps that inspect or list.\n"
         "- A plan whose only action is file_list, file_search, file_read, list_windows, proc_list, or sysinfo is NEVER a valid plan\n"
         "  for a goal that asks for change. Such inspection tools are fine as step 1 to GATHER state, but more steps must follow.\n"
         "- For bulk file operations (e.g. \"move all PDFs to X\"), use a single \"run\" step with find/xargs/mv -t rather than one step per file.\n"
         "- Prefer fewer, larger steps. Do not pad with verification steps unless the user asked for one.\n"
         "- Never add a step that tells the user what you plan to do — the goal is to DO, not narrate.\n"
         "- If the goal is simple enough for a single tool call (e.g. \"open firefox\"), emit a single step.\n"
         "- If the goal is genuinely conversational (no action), emit {\"steps\": []} and let the caller fall back.\n\n"
         "EXAMPLE — goal \"reorganize my Downloads folder by type\":\n"
         "{\"steps\":[\n"
         "  {\"tool\":\"run\",\"args\":{\"command\":\"mkdir -p ~/Downloads/PDFs ~/Downloads/Images ~/Downloads/Docs\"},\"expected\":\"subfolders created\"},\n"
         "  {\"tool\":\"run\",\"args\":{\"command\":\"find ~/Downloads -maxdepth 1 -type f -iname '*.pdf' -exec mv -t ~/Downloads/PDFs {} +\"},\"expected\":\"pdfs moved\"},\n"
         "  {\"tool\":\"run\",\"args\":{\"command\":\"find ~/Downloads -maxdepth 1 -type f \\\\( -iname '*.jpg' -o -iname '*.png' -o -iname '*.jpeg' \\\\) -exec mv -t ~/Downloads/Images {} +\"},\"expected\":\"images moved\"},\n"
         "  {\"tool\":\"run\",\"args\":{\"command\":\"find ~/Downloads -maxdepth 1 -type f \\\\( -iname '*.docx' -o -iname '*.pptx' -o -iname '*.xlsx' -o -iname '*.doc' -o -iname '*.ppt' \\\\) -exec mv -t ~/Downloads/Docs {} +\"},\"expected\":\"docs moved\"}\n"
         "]}\n\n"
         "AVAILABLE TOOLS:\n"
      << Planner::toolCatalogueText();
    return s.str();
}

std::string Planner::reflectSystemPrompt() {
    return
        "You are ARIA's planner reflection step. Given the current plan, the last executed step, and its "
        "observation, decide what to do next.\n\n"
        "OUTPUT FORMAT — strict JSON:\n"
        "{\"decision\": \"continue\" | \"replan\" | \"done\" | \"abort\", "
        "\"reason\": \"<one short sentence>\"}\n\n"
        "DECISION RULES:\n"
        "- \"continue\" — step succeeded and the plan still applies. Go to the next step.\n"
        "- \"replan\"   — step partially succeeded but observation invalidates later steps. Regenerate the tail.\n"
        "- \"done\"     — the USER'S STATED GOAL is already satisfied in the real world (e.g. every target file is in its "
                         "destination folder, the app is already open, the value is already set). A step merely COMPLETING "
                         "successfully is NOT 'done' unless that step's effect is the whole goal. A file_list / file_search / "
                         "list_* / inspection step NEVER satisfies a goal that asks to change state — always \"continue\" after those.\n"
        "- \"abort\"    — the step failed in a way that can't be recovered. The reason text will be spoken to the user.\n\n"
        "If this was not the last planned step, default to \"continue\" unless you have strong evidence to replan or abort.\n"
        "Be decisive. Prefer \"continue\" when the observation looks like normal success.";
}

std::string Planner::planToPromptText(const Plan& plan) {
    std::ostringstream s;
    s << "Goal: " << plan.goal << "\n";
    s << "Plan (" << plan.steps.size() << " steps):\n";
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        const auto& st = plan.steps[i];
        const char* tag = "·";
        switch (st.status) {
            case PlanStepStatus::Done:    tag = "✓"; break;
            case PlanStepStatus::Failed:  tag = "✗"; break;
            case PlanStepStatus::Skipped: tag = "»"; break;
            case PlanStepStatus::Denied:  tag = "⊘"; break;
            case PlanStepStatus::Running: tag = "»"; break;
            default:                       tag = "·"; break;
        }
        s << "  " << (i + 1) << ". " << tag << " " << st.tool
          << " " << st.args.dump();
        if (!st.observation.empty())
            s << "\n     → " << st.observation.substr(0, 200);
        s << "\n";
    }
    return s.str();
}

json Planner::extractPlanArray(const json& raw) {
    if (raw.is_object() && raw.contains("steps") && raw["steps"].is_array())
        return raw["steps"];
    if (raw.is_array())
        return raw;
    // Accept a single-step object as a courtesy.
    if (raw.is_object() && raw.contains("tool"))
        return json::array({raw});
    return json::array();
}

// ─── Destructive classification ───
//
// A step is destructive if it modifies external state the user would care
// about if it went wrong. The confirmation gate intercepts these before
// execution.
bool Planner::isDestructive(const PlanStep& step) {
    const auto& t = step.tool;
    if (t == "file_write")  return true;
    if (t == "proc_kill")   return true;
    if (t == "close")       return true;  // closes focused window — easy regret

    if (t == "run") {
        std::string cmd = step.args.value("command", "");
        std::string lower;
        lower.reserve(cmd.size());
        for (char c : cmd) lower.push_back(static_cast<char>(std::tolower(c)));
        // Heuristic deny list. Broad on purpose: we'd rather ask than lose data.
        static const char* triggers[] = {
            "sudo", " rm ", "rm -", " mv ", " dd ", "mkfs", ">/dev/", "shutdown",
            "reboot", "halt", "poweroff", "kill -9", "pkill", "pacman -r",
            "pacman -s ", "pacman -u", "chmod 000", ":(){", "fdisk",
        };
        for (const char* needle : triggers)
            if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ─── Planner::Planner ───

Planner::Planner(LLM* llm, Executor* executor, Memory* memory)
    : llm_(llm), executor_(executor), memory_(memory) {}

// ─── createPlan ───

Plan Planner::createPlan(const std::string& goal, const LLMContext& ctx) {
    Plan plan;
    plan.goal = goal;

    if (!llm_) return plan;

    std::ostringstream user;
    user << "Goal: " << goal << "\n";
    if (!ctx.activeApp.empty())    user << "Active app: " << ctx.activeApp << "\n";
    if (!ctx.activeWindow.empty()) user << "Window: "     << ctx.activeWindow << "\n";
    if (!ctx.clipboard.empty())    user << "Clipboard: "  << ctx.clipboard.substr(0, 200) << "\n";
    if (!ctx.memorySummary.empty()) user << ctx.memorySummary << "\n";

    auto raw = llm_->generateJson(planSystemPrompt(), user.str());
    auto arr = extractPlanArray(raw);

    for (auto& stepJson : arr) {
        if (!stepJson.is_object()) continue;
        PlanStep st = PlanStep::fromJson(stepJson);
        if (st.tool.empty()) continue;
        plan.steps.push_back(std::move(st));
        if (static_cast<int>(plan.steps.size()) >= kMaxSteps) break;
    }

    Logger::info("Planner: plan for \"" + goal + "\" → " +
                 std::to_string(plan.steps.size()) + " step(s)");
    return plan;
}

// ─── executeStep ───

std::string Planner::executeStep(PlanStep& step) {
    if (!executor_) { step.status = PlanStepStatus::Failed; return "No executor."; }
    step.status = PlanStepStatus::Running;

    AgentAction act{step.tool, step.args};
    std::string obs;

    if (step.tool == "run") {
        // Always route through safeShellCapture so the same deny list the
        // ReAct loop uses applies here too.
        obs = executor_->safeShellCapture(step.args.value("command", ""));
    } else {
        obs = executor_->execute(act);
    }

    // Trim to a reasonable length so it doesn't blow up the reflect prompt.
    if (obs.size() > 1500) obs = obs.substr(0, 1500) + "…[truncated]";
    step.observation = obs;

    // Heuristic: if the executor returned a string that clearly starts with
    // "Error" or "Failed" or "Refused", mark failed — otherwise done.
    std::string lower = obs;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    bool looksFailed =
        lower.rfind("error",    0) == 0 ||
        lower.rfind("failed",   0) == 0 ||
        lower.rfind("refused",  0) == 0 ||
        lower.rfind("invalid",  0) == 0;
    step.status = looksFailed ? PlanStepStatus::Failed : PlanStepStatus::Done;

    return obs;
}

// ─── reflect ───

std::string Planner::reflect(const Plan& plan, size_t stepIdx, const LLMContext& ctx) {
    if (!llm_ || stepIdx >= plan.steps.size()) return "continue";

    const auto& step = plan.steps[stepIdx];

    std::ostringstream user;
    user << "Goal: " << plan.goal << "\n\n";
    user << planToPromptText(plan) << "\n";
    user << "Just executed step " << (stepIdx + 1) << ": " << step.tool
         << " " << step.args.dump() << "\n";
    user << "Expected: " << step.expected << "\n";
    user << "Observed: " << step.observation << "\n";
    if (!ctx.activeApp.empty()) user << "Active app now: " << ctx.activeApp << "\n";

    bool isLast = (stepIdx + 1 >= plan.steps.size());
    if (isLast) user << "\nNote: this was the last planned step.\n";

    auto raw = llm_->generateJson(reflectSystemPrompt(), user.str());
    std::string decision = raw.value("decision", "continue");
    std::string reason   = raw.value("reason", "");

    // Normalize
    std::transform(decision.begin(), decision.end(), decision.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    if (decision == "abort") {
        return "abort:" + (reason.empty() ? "step failed" : reason);
    }

    // Hard guard: if the just-executed step was a pure-inspection tool, the
    // goal can't possibly be satisfied by it alone — override a too-eager
    // "done" to "continue" when later steps still exist.
    auto isInspectionOnly = [](const std::string& t) {
        return t == "file_list" || t == "file_search" || t == "file_read" ||
               t == "list_windows" || t == "proc_list"  || t == "sysinfo"   ||
               t == "read_screen_text" || t == "recall_memory" || t == "timer_list";
    };
    bool moreToDo = (stepIdx + 1 < plan.steps.size());
    if (decision == "done" && isInspectionOnly(step.tool) && moreToDo) {
        Logger::warn("Planner: overriding premature 'done' after inspection-only step '" +
                     step.tool + "' → continue");
        return "continue";
    }

    if (decision == "replan" || decision == "continue" || decision == "done")
        return decision;

    // Unknown decision — be conservative.
    Logger::warn("Planner: unknown reflect decision '" + decision + "', defaulting to continue");
    return "continue";
}

// ─── replan ───

bool Planner::replan(Plan& plan, size_t fromIdx, const LLMContext& ctx) {
    if (!llm_) return false;
    if (plan.usedReplans >= kMaxReplans) {
        Logger::warn("Planner: replan budget exhausted");
        return false;
    }
    ++plan.usedReplans;

    std::ostringstream user;
    user << "Original goal: " << plan.goal << "\n\n";
    user << "Progress so far:\n" << planToPromptText(plan) << "\n";
    user << "Regenerate ONLY the remaining steps (starting at step "
         << (fromIdx + 1) << "). Steps already executed stay as-is.\n";
    user << "Remaining budget: " << (kMaxSteps - static_cast<int>(fromIdx))
         << " steps.\n";
    (void)ctx;  // planning context-free for replans, observations carry the signal.

    auto raw = llm_->generateJson(planSystemPrompt(), user.str());
    auto arr = extractPlanArray(raw);

    // Truncate the existing tail, then append the new steps.
    plan.steps.resize(fromIdx);
    for (auto& stepJson : arr) {
        if (!stepJson.is_object()) continue;
        PlanStep st = PlanStep::fromJson(stepJson);
        if (st.tool.empty()) continue;
        plan.steps.push_back(std::move(st));
        if (static_cast<int>(plan.steps.size()) >= kMaxSteps) break;
    }

    Logger::info("Planner: replanned at step " + std::to_string(fromIdx + 1) +
                 " → " + std::to_string(plan.steps.size() - fromIdx) + " new step(s)");
    return plan.steps.size() > fromIdx;
}

// ─── run ───

PlanResult Planner::run(const std::string& goal,
                          const LLMContext& ctx,
                          ConfirmCallback   onConfirm,
                          ProgressCallback  onProgress,
                          AbortCheckCallback shouldAbort) {
    PlanResult res;

    if (onProgress) onProgress("Planning.");
    Plan plan = createPlan(goal, ctx);

    if (plan.steps.empty()) {
        res.summary = "I don't have a clear plan for that.";
        return res;
    }

    plan.deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(kWallTimeSec);

    // Persist a row up front so a crashed run still leaves a trace.
    if (memory_)
        plan.dbId = memory_->savePlan(goal, plan.toJson().dump(), "running");

    // ── Once-per-plan confirmation ──
    // Collect every destructive step's friendly description and ask ONE
    // voice question. Avoids the "yes / yes / yes" parade that made the
    // user give up on the previous attempt.
    std::vector<size_t> destructiveIdx;
    std::vector<std::string> destDesc;
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        if (isDestructive(plan.steps[i])) {
            destructiveIdx.push_back(i);
            destDesc.push_back(describeStep(plan.steps[i]));
        }
    }

    bool blanketConfirmed = destructiveIdx.empty();
    if (!destructiveIdx.empty()) {
        std::ostringstream prompt;
        // For 1–2 destructive steps, enumerate them so the user knows exactly
        // what they're approving. For 3+, switch to a one-line summary —
        // the previous version would read out 4-step plans verbatim and the
        // user would tune out before saying yes.
        if (destDesc.size() <= 2) {
            prompt << "I'll ";
            for (size_t i = 0; i < destDesc.size(); ++i) {
                if (i > 0) prompt << ", and ";
                prompt << destDesc[i];
            }
            prompt << ". Say yes to proceed.";
        } else {
            prompt << "I'll run a " << plan.steps.size()
                   << "-step plan with " << destDesc.size()
                   << " changes. Say yes to proceed.";
        }

        if (onConfirm) {
            blanketConfirmed = onConfirm(prompt.str(), plan.steps[destructiveIdx.front()]);
        } else {
            Logger::warn("Planner: destructive plan but no confirm callback — refusing");
        }
        if (!blanketConfirmed) {
            // Mark all destructive steps as denied; safe steps still run.
            for (size_t idx : destructiveIdx) {
                plan.steps[idx].status      = PlanStepStatus::Denied;
                plan.steps[idx].observation = "User declined plan.";
            }
            if (onProgress) onProgress("Skipping the parts that change files.");
        }
    }

    if (onProgress) {
        // Friendlier than "Plan: 4 steps." — describe step 1 so the user
        // hears something concrete.
        if (plan.steps.size() == 1) {
            onProgress("Going to " + describeStep(plan.steps.front()) + ".");
        } else {
            onProgress("Plan ready, " + std::to_string(plan.steps.size()) + " steps.");
        }
    }

    for (size_t i = 0; i < plan.steps.size(); ++i) {
        // Cooperative abort — checked every step so a "stop" voice command
        // or a Super+Space pause exits the plan within one step boundary.
        if (shouldAbort && shouldAbort()) {
            res.aborted     = true;
            res.abortReason = "User stop.";
            res.summary     = "Stopped.";
            Logger::info("Planner: aborted by user at step " + std::to_string(i + 1));
            break;
        }

        // Wall-clock guard
        if (std::chrono::steady_clock::now() > plan.deadline) {
            res.aborted     = true;
            res.abortReason = "Budget exceeded.";
            res.summary     = "Stopped — this was taking too long.";
            break;
        }

        auto& step = plan.steps[i];

        // Skip steps the blanket confirmation refused.
        if (step.status == PlanStepStatus::Denied) {
            Logger::info("Planner: step " + std::to_string(i + 1) +
                         " (" + step.tool + ") skipped — denied at plan-level confirm");
            continue;
        }

        // Belt-and-suspenders: if a step came back marked destructive but
        // wasn't covered by the blanket prompt (e.g. introduced by a replan),
        // ask once for it specifically.
        if (isDestructive(step) && !blanketConfirmed) {
            std::string ad = "About to " + describeStep(step) + ". Say yes to proceed.";
            bool ok = onConfirm ? onConfirm(ad, step) : false;
            if (!ok) {
                step.status      = PlanStepStatus::Denied;
                step.observation = "User declined.";
                if (onProgress) onProgress("Skipped that one.");
                continue;
            }
        }

        if (onProgress) {
            // Voice-friendly: "Step 2: moving images." not "Step 2 of 4: run."
            onProgress("Step " + std::to_string(i + 1) + ": " + describeStep(step) + ".");
        }

        executeStep(step);
        ++res.stepsExecuted;

        Logger::info("Planner step " + std::to_string(i + 1) + "/" +
                     std::to_string(plan.steps.size()) + ": " + step.tool +
                     " → status=" +
                     (step.status == PlanStepStatus::Done ? "done" : "failed"));

        // Second abort check after the step runs — long shell commands could
        // have used up the user's patience while we were busy.
        if (shouldAbort && shouldAbort()) {
            res.aborted     = true;
            res.abortReason = "User stop.";
            res.summary     = "Stopped.";
            break;
        }

        std::string decision = reflect(plan, i, ctx);

        if (decision == "done") {
            res.success = true;
            res.summary = "Done.";
            break;
        }
        if (decision.rfind("abort:", 0) == 0) {
            res.aborted     = true;
            res.abortReason = decision.substr(6);
            res.summary     = "Stopped — " + res.abortReason;
            break;
        }
        if (decision == "replan") {
            if (!replan(plan, i + 1, ctx)) {
                res.aborted     = true;
                res.abortReason = "Replan failed.";
                res.summary     = "Couldn't adjust the plan.";
                break;
            }
        }
        // "continue" → falls through to next iteration
    }

    // Determine final outcome if nothing set it above.
    if (!res.aborted && res.summary.empty()) {
        bool anyFailed = false;
        for (const auto& st : plan.steps) {
            if (st.status == PlanStepStatus::Failed) { anyFailed = true; break; }
        }
        res.success = !anyFailed;
        res.summary = res.success
            ? "Done with " + std::to_string(res.stepsExecuted) + " step" +
              (res.stepsExecuted == 1 ? "." : "s.")
            : "Finished with problems on some steps.";
    }

    // Persist final state.
    if (memory_ && plan.dbId > 0) {
        std::string outcome = res.aborted ? "aborted"
                              : (res.success ? "success" : "partial");
        if (!res.abortReason.empty()) outcome += ": " + res.abortReason;
        memory_->updatePlanOutcome(plan.dbId, outcome, plan.toJson().dump());
    }

    return res;
}
