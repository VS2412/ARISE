#include "cortex/trace_scorer.hpp"

#include "cortex/sub_agent.hpp"

#include <cmath>
#include <sstream>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace arise {

namespace {

constexpr const char* kJudgePrompt =
    "You are ARISE's nightly self-evaluator. Score ARISE's response on a "
    "single user turn. Reply with JSON ONLY, no prose, no markdown:\n"
    "{\n"
    "  \"tool_correctness\": <0..1>,\n"
    "  \"tone_match\":       <0..1>,\n"
    "  \"memory_use\":       <0..1>,\n"
    "  \"overall\":          <0..1>,\n"
    "  \"reasoning\":         \"<one short phrase>\"\n"
    "}\n"
    "Rubric:\n"
    "  tool_correctness — was the right tool / suggestion type chosen?\n"
    "  tone_match       — did the tone fit the user's mood + situation?\n"
    "  memory_use       — did ARISE bring useful context (or skip rehash)?\n"
    "  overall          — your aggregate gut score.\n"
    "If any axis is N/A, give 0.5.";

double clamp01(double x) {
    if (std::isnan(x)) return 0.0;
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

double readNumber(const json& j, const char* key, double fallback = 0.5) {
    if (!j.contains(key)) return fallback;
    if (j[key].is_number()) return clamp01(j[key].get<double>());
    if (j[key].is_string()) {
        try { return clamp01(std::stod(j[key].get<std::string>())); }
        catch (...) { return fallback; }
    }
    return fallback;
}

} // namespace

std::optional<TraceScorer::Score>
TraceScorer::parse(std::string_view text, bool strip_thinking) {
    auto cleaned = strip_thinking ? SubAgent::stripThinkingBlocks(text)
                                   : std::string(text);
    auto blob = SubAgent::firstJsonObject(cleaned);
    if (!blob) return std::nullopt;
    try {
        auto j = json::parse(*blob);
        Score s;
        s.tool_correctness = readNumber(j, "tool_correctness");
        s.tone_match       = readNumber(j, "tone_match");
        s.memory_use       = readNumber(j, "memory_use");
        s.overall          = readNumber(j, "overall");
        if (j.contains("reasoning") && j["reasoning"].is_string())
            s.reasoning = j["reasoning"].get<std::string>();
        s.ok       = true;
        s.from_llm = true;
        return s;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

TraceScorer::TraceScorer(Config cfg) : cfg_(std::move(cfg)) {}

TraceScorer::Score TraceScorer::score(const Trace& t) const {
    Score fallback;
    fallback.tool_correctness = 0.5;
    fallback.tone_match       = 0.5;
    fallback.memory_use       = 0.5;
    fallback.overall          = 0.5;
    fallback.from_llm         = false;

    if (t.user_input.empty() && t.response_summary.empty()) return fallback;

    SubAgent::Config lc;
    lc.role          = "trace_scorer";
    lc.model         = cfg_.model;
    lc.ollama_url    = cfg_.ollama_url;
    lc.num_gpu       = cfg_.num_gpu;
    lc.format_json   = true;
    lc.timeout_sec   = cfg_.timeout_sec;
    lc.max_predict   = 200;
    lc.strip_thinking = cfg_.strip_thinking;
    lc.system_prompt  = kJudgePrompt;
    SubAgent agent(lc);

    std::ostringstream task;
    task << "USER_INPUT:\n" << t.user_input << "\n\n"
         << "ARISE_RESPONSE_KIND: " << t.response_kind << "\n"
         << "ARISE_RESPONSE: " << t.response_summary << "\n\n"
         << "OUTCOME: ";
    if (t.outcome) task << decisionToString(*t.outcome);
    else           task << "(no decision)";
    task << "\nReply with JSON only.";

    auto r = agent.run(task.str());
    if (!r.ok) return fallback;
    auto parsed = parse(r.output, cfg_.strip_thinking);
    if (!parsed) return fallback;
    reachable_.store(true, std::memory_order_relaxed);
    return *parsed;
}

} // namespace arise
