#include "cortex/training_curator.hpp"

#include "cortex/feedback_db.hpp"
#include "cortex/memory_cortex.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

std::string nowSlug() {
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm_buf{}; gmtime_r(&t, &tm_buf);
    char buf[32]; std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm_buf);
    return buf;
}

} // namespace

TrainingCurator::TrainingCurator(Config cfg) : cfg_(std::move(cfg)) {}

std::vector<TrainingExample>
TrainingCurator::curate(const MemoryCortex& cortex, const FeedbackDb& feedback,
                        Stats* out_stats) const {
    Stats stats;

    TraceReplay replay(cfg_.replay);
    auto traces = replay.replay(cortex, feedback);
    stats.replayed_total = traces.size();

    PrivacyFilter privacy(cfg_.privacy);
    TraceScorer   scorer(cfg_.scorer);

    std::vector<TrainingExample> ok_examples;
    ok_examples.reserve(traces.size());
    for (const auto& t : traces) {
        if (t.user_input.empty() || t.response_summary.empty()) {
            ++stats.dropped_empty;
            continue;
        }

        TraceScorer::Score s;
        if (cfg_.skip_llm_scoring) {
            s.ok               = true;
            s.from_llm         = false;
            s.tool_correctness = 0.5;
            s.tone_match       = 0.5;
            s.memory_use       = 0.5;
            s.overall          = 0.5;
        } else {
            s = scorer.score(t);
            ++stats.scored;
            if (s.overall < cfg_.scorer.drop_below_overall) {
                ++stats.dropped_low_score;
                continue;
            }
        }

        auto pu = privacy.scan(t.user_input);
        auto pr = privacy.scan(t.response_summary);
        if (!pu.passed || !pr.passed) {
            ++stats.dropped_privacy;
            continue;
        }

        TrainingExample ex;
        ex.trace                 = t;
        ex.score                 = s;
        ex.sanitised_user_input  = pu.redacted_text;
        ex.sanitised_response    = pr.redacted_text;
        ok_examples.push_back(std::move(ex));
    }

    // Top-N by overall score, descending. Ties broken by recency (later first).
    std::sort(ok_examples.begin(), ok_examples.end(),
              [](const TrainingExample& a, const TrainingExample& b) {
                  if (a.score.overall != b.score.overall)
                      return a.score.overall > b.score.overall;
                  return a.trace.user_at > b.trace.user_at;
              });
    if (cfg_.top_n > 0 && int(ok_examples.size()) > cfg_.top_n) {
        ok_examples.resize(cfg_.top_n);
    }
    stats.kept = ok_examples.size();
    if (out_stats) *out_stats = stats;
    return ok_examples;
}

std::string
TrainingCurator::writeJsonl(const std::vector<TrainingExample>& examples,
                            const std::string& dir) {
    if (dir.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};

    auto path = dir + "/" + nowSlug() + ".jsonl";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return {};

    for (const auto& ex : examples) {
        json j;
        j["user_input"]   = ex.sanitised_user_input;
        j["response"]     = ex.sanitised_response;
        j["response_kind"] = ex.trace.response_kind;
        if (ex.trace.outcome)
            j["outcome"] = decisionToString(*ex.trace.outcome);
        j["scores"] = {
            {"tool_correctness", ex.score.tool_correctness},
            {"tone_match",       ex.score.tone_match},
            {"memory_use",       ex.score.memory_use},
            {"overall",          ex.score.overall},
            {"from_llm",         ex.score.from_llm},
        };
        if (!ex.trace.source_device.empty())
            j["source_device"] = ex.trace.source_device;
        f << j.dump() << "\n";
    }
    return path;
}

} // namespace arise
