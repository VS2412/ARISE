#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace arise {

class Blackboard;
class DeviceStore;
class FeedbackDb;
class GoalStore;
class MemoryCortex;

// Inbound event-router for shards (phone / tablet / overlay / mqtt). Phase
// 7 commit 1 owns the protocol layer; the WebSocket / HTTP carrier lands
// in commit 2. By keeping the router carrier-agnostic, both the network
// listener and the CLI's `arise federation ingest` exercise the exact same
// code path.
//
// Inbound event shape:
//   { "type": "utterance" | "decision" | "goal_query" | "ping",
//     "source_device": "<id>",        // optional — derived from token if absent
//     "ts": <epoch seconds>,           // optional — set by router if missing
//     "payload": { ... type-specific ... } }
//
// All inbound events authenticate via the presented plaintext token (NOT
// stored anywhere — only its sha256 lives on disk). Per-event permissions
// are enforced before dispatch.
//
// Outputs:
//   * Synchronous: a JSON `Response` carrying `ok`, `error`, and (for
//     query-type events) a structured payload.
//   * Asynchronous: blackboard topics `federation.utterance` /
//     `federation.decision` / `federation.goal_query` / `federation.ping`,
//     each tagged with `source_device` so downstream systems (Phase 4
//     orchestrator, Phase 6 proactive) can route + remember per-device.
//   * Episodic: utterances are mirrored as a `federation_utterance`
//     EpisodicEvent so `arise mem recall` finds them.
class FederationRouter {
public:
    struct Config {
        DeviceStore*    devices = nullptr;     // required
        Blackboard*     bb       = nullptr;
        MemoryCortex*   cortex   = nullptr;
        FeedbackDb*     feedback = nullptr;    // for `decision` events
        GoalStore*      goals    = nullptr;    // for `goal_query` responses

        // Hard cap on inbound payload size to keep a misbehaving shard
        // from spamming. Server checks `event.dump().size()`.
        std::size_t     max_event_bytes = 64 * 1024;
        // Reject events whose `ts` differs from the server clock by more
        // than this. Zero = no check.
        std::chrono::seconds clock_skew_tolerance { 5 * 60 };
    };

    enum class Code {
        Ok,
        Unauthorized,
        Forbidden,
        BadRequest,
        InternalError,
    };

    static const char* codeToString(Code c);

    struct Response {
        bool        ok = false;
        Code        code = Code::InternalError;
        std::string error;            // human-readable
        nlohmann::json payload = nlohmann::json::object();
        std::string event_type;        // echoed for the carrier layer
        std::string source_device;     // resolved from token
    };

    explicit FederationRouter(Config cfg);

    // Process one inbound event. `presented_token` is the plaintext token
    // received from the shard. Never logged. The router resolves the
    // device, checks per-event permissions, runs the handler, and returns
    // a Response the carrier layer turns into a WS frame / HTTP body.
    Response ingest(const nlohmann::json& event,
                    const std::string& presented_token);

    // Convenience: parse a JSON string then ingest. Common path for the WS
    // / HTTP carrier and for the CLI.
    Response ingestRaw(std::string_view raw_json,
                       const std::string& presented_token);

    const Config& config() const { return cfg_; }

private:
    Config cfg_;

    // Per-type handlers. Each returns `(code, message, payload)`.
    Response handleUtterance_(const std::string& source_device,
                              const nlohmann::json& payload);
    Response handleDecision_ (const std::string& source_device,
                              const nlohmann::json& payload);
    Response handleGoalQuery_(const std::string& source_device,
                              const nlohmann::json& payload);
    Response handlePing_     (const std::string& source_device,
                              const nlohmann::json& payload);
};

} // namespace arise
