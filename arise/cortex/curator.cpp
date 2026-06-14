#include "cortex/curator.hpp"

#include "blackboard/blackboard.hpp"
#include "cortex/memory_cortex.hpp"
#include "cortex/sub_agent.hpp"
#include "util/log.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace std::chrono;

namespace arise {

namespace {

constexpr const char* kSystemPrompt =
    "You are ARISE's Curator. Read the user/assistant transcript and extract "
    "STABLE FACTS about the user that are worth remembering long-term. "
    "Skip ephemeral details (today's mood, what they're working on right now). "
    "Reply with JSON only, no prose, no markdown — exactly:\n"
    "{\"facts\": [{\"subject\":\"<who>\",\"predicate\":\"<verb_phrase>\","
    "\"object\":\"<value>\",\"confidence\":<0..1>}, ...]}\n"
    "If nothing is worth keeping, reply {\"facts\": []}.";

double clamp01(double x) {
    if (std::isnan(x)) return 0.0;
    if (x < 0.0)       return 0.0;
    if (x > 1.0)       return 1.0;
    return x;
}

std::string trimToTail(const std::string& s, int max_chars) {
    if (max_chars <= 0 || int(s.size()) <= max_chars) return s;
    // Keep the tail. Find a newline boundary in the discard window so we don't
    // chop a sentence — fall back to a hard cut if no newline is nearby.
    auto start = s.size() - max_chars;
    auto nl = s.find('\n', start);
    if (nl != std::string::npos && nl < start + 256) start = nl + 1;
    return std::string("[…transcript truncated…]\n") + s.substr(start);
}

} // namespace

// ─── pure parser ───────────────────────────────────────────────────────────

std::vector<Curator::ExtractedFact>
Curator::parseFacts(const std::string& llm_text, int max_facts) {
    std::vector<ExtractedFact> out;
    auto cleaned = SubAgent::stripThinkingBlocks(llm_text);
    auto blob    = SubAgent::firstJsonObject(cleaned);
    if (!blob) return out;

    try {
        auto j = json::parse(*blob);
        if (!j.contains("facts") || !j["facts"].is_array()) return out;
        for (const auto& f : j["facts"]) {
            if (!f.is_object()) continue;
            ExtractedFact ef;
            if (f.contains("subject")   && f["subject"].is_string())
                ef.subject = f["subject"].get<std::string>();
            if (f.contains("predicate") && f["predicate"].is_string())
                ef.predicate = f["predicate"].get<std::string>();
            if (f.contains("object")    && f["object"].is_string())
                ef.object = f["object"].get<std::string>();
            if (f.contains("confidence")) {
                if (f["confidence"].is_number()) {
                    ef.confidence = clamp01(f["confidence"].get<double>());
                } else if (f["confidence"].is_string()) {
                    try { ef.confidence = clamp01(std::stod(
                              f["confidence"].get<std::string>())); }
                    catch (...) { ef.confidence = 0.5; }
                }
            }
            if (ef.subject.empty() || ef.predicate.empty() || ef.object.empty())
                continue;
            out.push_back(std::move(ef));
            if (max_facts > 0 && int(out.size()) >= max_facts) break;
        }
    } catch (const std::exception&) {
        // best-effort; drop silently
    }
    return out;
}

// ─── impl / lifecycle ──────────────────────────────────────────────────────

struct Curator::Impl {
    Config cfg;

    std::atomic<bool> running { false };
    std::atomic<bool> stopping{ false };
    std::thread       worker;

    Blackboard::Subscription sub;

    mutable std::mutex stats_mu;
    Stats              stats;
};

Curator::Curator(Config cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = std::move(cfg);
    if (!p_->cfg.cortex) log::error("Curator: cortex is required");
    if (!p_->cfg.llm)    log::error("Curator: llm is required");
}
Curator::~Curator() { stop(); }

bool Curator::running() const { return p_->running.load(); }
Curator::Stats Curator::stats() const {
    std::lock_guard<std::mutex> lk(p_->stats_mu);
    return p_->stats;
}

void Curator::start() {
    if (!p_->cfg.bb || !p_->cfg.cortex || !p_->cfg.llm) return;
    bool expected = false;
    if (!p_->running.compare_exchange_strong(expected, true)) return;
    p_->stopping.store(false);
    p_->sub = p_->cfg.bb->subscribe("conversation.closed");
    p_->worker = std::thread(&Curator::workerLoop_, this);
    log::info("Curator: started");
}

void Curator::stop() {
    if (!p_) return;
    bool was_running = p_->running.exchange(false);
    p_->stopping.store(true);
    if (p_->sub.valid()) p_->sub.stop();
    if (p_->worker.joinable()) p_->worker.join();
    if (was_running) log::info("Curator: stopped");
}

void Curator::workerLoop_() {
    while (!p_->stopping.load()) {
        auto ev = p_->sub.next(milliseconds(250));
        if (!ev) continue;
        if (ev->topic != "conversation.closed") continue;
        if (!ev->payload.contains("transcript")
            || !ev->payload["transcript"].is_string()) continue;
        std::string transcript = ev->payload["transcript"].get<std::string>();
        absorbConversation(transcript);
    }
}

// ─── absorbConversation ────────────────────────────────────────────────────

int Curator::absorbConversation(const std::string& transcript,
                                std::vector<ExtractedFact>* out_facts) {
    if (out_facts) out_facts->clear();
    if (!p_->cfg.cortex || !p_->cfg.llm) return 0;
    if (transcript.empty()) return 0;

    { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.conversations_seen; }

    auto prompt_body = trimToTail(transcript, p_->cfg.max_transcript_chars);
    std::string task =
        std::string(kSystemPrompt) + "\n\nTRANSCRIPT:\n" + prompt_body
        + "\n\nReply with JSON only.";

    { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.llm_calls; }
    auto r = p_->cfg.llm->run(task);
    if (!r.ok) {
        log::warn(std::string("Curator: llm: ")
                  + (r.error.empty() ? "(no error)" : r.error));
        std::lock_guard<std::mutex> lk(p_->stats_mu);
        ++p_->stats.llm_failures;
        return 0;
    }

    auto facts = parseFacts(r.output, p_->cfg.max_facts_per_call);
    {
        std::lock_guard<std::mutex> lk(p_->stats_mu);
        p_->stats.facts_extracted += facts.size();
    }
    if (out_facts) *out_facts = facts;

    int upserted = 0;
    for (const auto& f : facts) {
        if (f.confidence < p_->cfg.min_confidence) {
            std::lock_guard<std::mutex> lk(p_->stats_mu);
            ++p_->stats.facts_rejected;
            continue;
        }
        SemanticFact sf;
        sf.subject    = f.subject;
        sf.predicate  = f.predicate;
        sf.object     = f.object;
        sf.confidence = f.confidence;
        if (p_->cfg.cortex->upsertFact(std::move(sf)) > 0) {
            ++upserted;
            std::lock_guard<std::mutex> lk(p_->stats_mu);
            ++p_->stats.facts_upserted;
        } else {
            std::lock_guard<std::mutex> lk(p_->stats_mu);
            ++p_->stats.facts_rejected;
        }
    }

    if (p_->cfg.bb) {
        json payload;
        payload["count"] = upserted;
        json arr = json::array();
        for (const auto& f : facts) {
            arr.push_back({
                {"subject",    f.subject},
                {"predicate",  f.predicate},
                {"object",     f.object},
                {"confidence", f.confidence},
            });
        }
        payload["facts"] = std::move(arr);
        p_->cfg.bb->publish("agent.curator.facts", payload);
    }

    return upserted;
}

} // namespace arise
