#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "cortex/sub_agent.hpp"

namespace arise {

class Blackboard;
class MemoryCortex;

// "What did this conversation tell us about the user?" agent.
//
// Subscribes to `conversation.closed` events on the blackboard. On each one,
// reads the transcript out of the payload, asks a tiny qwen3:0.6b to extract
// stable subject-predicate-object facts as JSON, and upserts them into the
// MemoryCortex's semantic store. Contradiction-aware upsert is already
// handled by `MemoryCortex::upsertFact`, so the Curator just feeds it.
//
// Direct entry point `absorbConversation()` lets the CLI and tests skip the
// blackboard.
class Curator {
public:
    struct ExtractedFact {
        std::string subject;
        std::string predicate;
        std::string object;
        double      confidence = 0.5;
    };

    struct Config {
        Blackboard*    bb     = nullptr;          // optional — auto-subscribe path
        MemoryCortex*  cortex = nullptr;          // required — fact sink
        SubAgent*      llm    = nullptr;          // required — fact extractor

        int    max_facts_per_call = 10;
        double min_confidence     = 0.5;          // drop below this
        // Keep transcripts manageable for qwen3:0.6b's context. Long ones are
        // truncated from the head (oldest) so the close-of-conversation tail
        // — usually where the most distilled facts live — stays intact.
        int    max_transcript_chars = 6000;
    };

    explicit Curator(Config cfg);
    ~Curator();
    Curator(const Curator&)            = delete;
    Curator& operator=(const Curator&) = delete;

    void start();
    void stop();
    bool running() const;

    struct Stats {
        std::size_t conversations_seen = 0;
        std::size_t facts_extracted    = 0;
        std::size_t facts_upserted     = 0;
        std::size_t facts_rejected     = 0;       // below confidence or invalid
        std::size_t llm_calls          = 0;
        std::size_t llm_failures       = 0;
    };
    Stats stats() const;

    // Synchronous fact extraction + upsert. Returns the count of facts that
    // landed in the semantic store (≤ extracted ≤ max_facts_per_call). Used
    // both by the worker loop and by the CLI / tests.
    int absorbConversation(const std::string& transcript,
                           std::vector<ExtractedFact>* out_facts = nullptr);

    // Pure parser exposed for tests: pulls a `{"facts":[...]}` JSON object
    // out of arbitrary LLM text. Strips qwen3 <think> blocks first.
    static std::vector<ExtractedFact> parseFacts(const std::string& llm_text,
                                                 int max_facts);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;

    void workerLoop_();
};

} // namespace arise
