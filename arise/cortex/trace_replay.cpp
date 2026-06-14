#include "cortex/trace_replay.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

bool kindMatches(const std::vector<std::string>& list,
                 const std::string& kind) {
    return std::find(list.begin(), list.end(), kind) != list.end();
}

std::string extractSourceDevice(const std::string& payload_json) {
    if (payload_json.empty()) return {};
    try {
        auto j = json::parse(payload_json);
        if (j.contains("source_device") && j["source_device"].is_string()) {
            return j["source_device"].get<std::string>();
        }
    } catch (...) {}
    return {};
}

// Pull `text` (federation_utterance) or `summary` itself for ARIA conversations.
std::string extractUserText(const EpisodicEvent& ev) {
    if (!ev.payload_json.empty()) {
        try {
            auto j = json::parse(ev.payload_json);
            if (j.contains("text") && j["text"].is_string()) {
                return j["text"].get<std::string>();
            }
        } catch (...) {}
    }
    return ev.summary;
}

// For a proactive_suggestion mirror, dig the suggestion id out of the payload.
std::optional<std::int64_t> extractSuggestionId(const std::string& payload_json) {
    if (payload_json.empty()) return std::nullopt;
    try {
        auto j = json::parse(payload_json);
        if (j.contains("id") && j["id"].is_number_integer()) {
            return j["id"].get<std::int64_t>();
        }
    } catch (...) {}
    return std::nullopt;
}

} // namespace

TraceReplay::TraceReplay(Config cfg) : cfg_(std::move(cfg)) {}

std::vector<Trace> TraceReplay::replay(const MemoryCortex& cortex,
                                       const FeedbackDb&  feedback) const {
    auto cutoff = system_clock::now() - cfg_.lookback;

    auto events = cortex.recentEvents(cfg_.max_events);
    // recentEvents() returns newest-first — flip to chronological so we can
    // pair user_input with the *next* response.
    std::reverse(events.begin(), events.end());

    // Index feedback decisions by suggestion id for outcome lookup.
    FeedbackQuery fq; fq.limit = 5000;
    auto fb_rows = feedback.list(fq);

    auto find_outcome = [&](std::int64_t suggestion_id)
        -> std::pair<std::optional<Decision>, std::optional<Timestamp>> {
        for (const auto& row : fb_rows) {
            if (row.id == suggestion_id) {
                if (row.decision != Decision::Pending)
                    return { row.decision, row.decided_at };
                return { row.decision, std::nullopt };
            }
        }
        return { std::nullopt, std::nullopt };
    };

    std::vector<Trace> out;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& ev = events[i];
        if (ev.ts < cutoff) continue;
        if (!kindMatches(cfg_.user_kinds, ev.kind)) continue;

        Trace t;
        t.id            = ev.id;
        t.source_device = extractSourceDevice(ev.payload_json);
        t.user_input    = extractUserText(ev);
        t.user_at       = ev.ts;

        // Walk forward looking for the first response within `response_window`.
        for (std::size_t j = i + 1; j < events.size(); ++j) {
            const auto& cand = events[j];
            if (cand.ts - ev.ts > cfg_.response_window) break;
            if (!kindMatches(cfg_.response_kinds, cand.kind)) continue;

            t.response_kind         = cand.kind;
            t.response_summary      = cand.summary;
            t.response_payload_json = cand.payload_json;
            t.response_at           = cand.ts;
            t.suggestion_id         = extractSuggestionId(cand.payload_json);
            if (t.suggestion_id) {
                auto [decision, decided_at] = find_outcome(*t.suggestion_id);
                t.outcome    = decision;
                t.outcome_at = decided_at;
            }
            break;
        }

        out.push_back(std::move(t));
    }
    return out;
}

} // namespace arise
