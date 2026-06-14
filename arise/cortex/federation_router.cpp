#include "cortex/federation_router.hpp"

#include "blackboard/blackboard.hpp"
#include "cortex/device_store.hpp"
#include "cortex/feedback_db.hpp"
#include "cortex/goals.hpp"
#include "cortex/memory_cortex.hpp"
#include "util/log.hpp"

#include <chrono>

using nlohmann::json;
using namespace std::chrono;

namespace arise {

const char* FederationRouter::codeToString(Code c) {
    switch (c) {
        case Code::Ok:            return "ok";
        case Code::Unauthorized:  return "unauthorized";
        case Code::Forbidden:     return "forbidden";
        case Code::BadRequest:    return "bad_request";
        case Code::InternalError: return "internal_error";
    }
    return "internal_error";
}

namespace {

FederationRouter::Response err(FederationRouter::Code c, std::string msg) {
    FederationRouter::Response r;
    r.ok = false; r.code = c; r.error = std::move(msg);
    return r;
}

FederationRouter::Response ok(json payload = json::object()) {
    FederationRouter::Response r;
    r.ok = true; r.code = FederationRouter::Code::Ok;
    r.payload = std::move(payload);
    return r;
}

} // namespace

FederationRouter::FederationRouter(Config cfg) : cfg_(std::move(cfg)) {
    if (!cfg_.devices) log::error("FederationRouter: devices is required");
}

FederationRouter::Response
FederationRouter::ingest(const json& event,
                         const std::string& presented_token) {
    if (!cfg_.devices) return err(Code::InternalError, "router: no device store");
    if (!event.is_object())
        return err(Code::BadRequest, "event must be a JSON object");
    if (cfg_.max_event_bytes > 0
        && event.dump().size() > cfg_.max_event_bytes) {
        return err(Code::BadRequest, "event exceeds max_event_bytes");
    }

    // Authenticate first — never reveal whether the type / payload was
    // valid before knowing the caller is allowed to ask.
    auto device = cfg_.devices->getByToken(presented_token);
    if (!device) return err(Code::Unauthorized, "invalid token");

    auto type = event.value("type", std::string{});
    if (type.empty()) return err(Code::BadRequest, "missing 'type'");

    auto declared_source = event.value("source_device", std::string{});
    auto resolved_source = device->id;
    if (!declared_source.empty() && declared_source != resolved_source) {
        return err(Code::Forbidden, "source_device mismatch with token");
    }

    // Optional clock-skew check.
    if (cfg_.clock_skew_tolerance.count() > 0 && event.contains("ts")
        && event["ts"].is_number_integer()) {
        long long server_now = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count();
        long long client_ts  = event["ts"].get<long long>();
        long long skew = (server_now > client_ts)
            ? (server_now - client_ts) : (client_ts - server_now);
        if (skew > cfg_.clock_skew_tolerance.count()) {
            return err(Code::BadRequest, "clock skew exceeds tolerance");
        }
    }

    auto payload = event.contains("payload") ? event["payload"]
                                              : json::object();

    Response r;
    if      (type == "utterance" ) {
        if (!device->perms.can_utterance)    return err(Code::Forbidden, "utterance not permitted");
        r = handleUtterance_(resolved_source, payload);
    }
    else if (type == "decision"  ) {
        if (!device->perms.can_decision)     return err(Code::Forbidden, "decision not permitted");
        r = handleDecision_(resolved_source, payload);
    }
    else if (type == "goal_query") {
        if (!device->perms.can_goal_query)   return err(Code::Forbidden, "goal_query not permitted");
        r = handleGoalQuery_(resolved_source, payload);
    }
    else if (type == "ping")        r = handlePing_(resolved_source, payload);
    else return err(Code::BadRequest, "unknown event type '" + type + "'");

    r.event_type    = type;
    r.source_device = resolved_source;
    if (r.ok) cfg_.devices->recordSeen(device->id);
    return r;
}

FederationRouter::Response
FederationRouter::ingestRaw(std::string_view raw_json,
                            const std::string& presented_token) {
    json j;
    try {
        j = json::parse(raw_json);
    } catch (const std::exception& e) {
        return err(Code::BadRequest,
                   std::string("invalid JSON: ") + e.what());
    }
    return ingest(j, presented_token);
}

// ─── handlers ──────────────────────────────────────────────────────────────

FederationRouter::Response
FederationRouter::handleUtterance_(const std::string& source_device,
                                   const json& payload) {
    auto text = payload.value("text", std::string{});
    if (text.empty()) return err(Code::BadRequest, "missing payload.text");
    auto modality = payload.value("modality", std::string{"text"});

    if (cfg_.bb) {
        json out;
        out["text"]          = text;
        out["modality"]      = modality;
        out["source_device"] = source_device;
        cfg_.bb->publish("federation.utterance", out);
    }
    if (cfg_.cortex) {
        EpisodicEvent ev;
        ev.kind     = "federation_utterance";
        ev.summary  = "[" + source_device + "] " + text;
        // Speech from the user is high-salience by default.
        ev.salience = 0.65;
        json p; p["text"] = text; p["modality"] = modality;
        p["source_device"] = source_device;
        ev.payload_json = p.dump();
        cfg_.cortex->recordEvent(std::move(ev));
    }
    return ok(json{
        {"accepted", true},
        {"echo_text", text},
    });
}

FederationRouter::Response
FederationRouter::handleDecision_(const std::string& source_device,
                                  const json& payload) {
    if (!cfg_.feedback) return err(Code::InternalError, "no feedback db wired");
    if (!payload.contains("id") || !payload["id"].is_number_integer()) {
        return err(Code::BadRequest, "missing payload.id");
    }
    auto id = payload["id"].get<std::int64_t>();
    auto decision_str = payload.value("decision", std::string{});
    auto d = decisionFromString(decision_str);
    if (!d || *d == Decision::Pending) {
        return err(Code::BadRequest, "decision must be accepted/rejected/ignored/timedout");
    }
    bool wrote = cfg_.feedback->recordDecision(id, *d);
    if (!wrote) return err(Code::BadRequest, "decision rejected (id missing or already terminal)");

    if (cfg_.bb) {
        json out;
        out["id"]            = id;
        out["decision"]      = decisionToString(*d);
        out["source_device"] = source_device;
        cfg_.bb->publish("federation.decision", out);
    }
    return ok(json{{"id", id}, {"decision", decisionToString(*d)}});
}

FederationRouter::Response
FederationRouter::handleGoalQuery_(const std::string& source_device,
                                   const json& payload) {
    if (!cfg_.goals) return err(Code::InternalError, "no goal store wired");

    GoalQuery gq;
    gq.limit = payload.value("limit", 10);
    auto status_str = payload.value("status", std::string{"in_progress"});
    if (auto s = goalStatusFromString(status_str)) gq.status = s;
    auto rows = cfg_.goals->list(gq);

    json arr = json::array();
    for (const auto& g : rows) {
        json gj;
        gj["id"]       = g.id;
        gj["summary"]  = g.summary;
        gj["status"]   = toString(g.status);
        gj["priority"] = g.priority;
        if (g.deadline_at) {
            gj["deadline_epoch"] = duration_cast<seconds>(
                                        g.deadline_at->time_since_epoch()).count();
        }
        if (!g.tags.empty()) gj["tags"] = g.tags;
        arr.push_back(std::move(gj));
    }

    if (cfg_.bb) {
        json out;
        out["count"]         = arr.size();
        out["source_device"] = source_device;
        cfg_.bb->publish("federation.goal_query", out);
    }
    return ok(json{{"goals", std::move(arr)}});
}

FederationRouter::Response
FederationRouter::handlePing_(const std::string& source_device,
                              const json& /*payload*/) {
    if (cfg_.bb) {
        json out; out["source_device"] = source_device;
        out["server_ts"] = duration_cast<seconds>(
                              system_clock::now().time_since_epoch()).count();
        cfg_.bb->publish("federation.ping", out);
    }
    return ok(json{
        {"pong", true},
        {"server_ts", duration_cast<seconds>(
                          system_clock::now().time_since_epoch()).count()},
    });
}

} // namespace arise
