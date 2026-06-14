#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace arise {

// Phase 4 commit 2. Generic, role-agnostic handle to a sub-agent worker
// running on a detached `std::thread`. The handle is cheap to copy (it's a
// shared_ptr around the state) and survives independently from the
// Orchestrator that minted it — so the user can `wait()` even if the
// orchestrator is destructed first.
//
// The kill mechanism is cooperative: the worker periodically checks
// `state->kill_requested.load()` and bails out cleanly. SubAgent::run()
// calls already in flight can't be aborted (curl owns the socket) but the
// next iteration of any ReAct loop / tool dispatch checks the flag before
// kicking off another LLM round-trip.
class SpawnHandle {
public:
    enum class State {
        Pending,    // thread not yet started
        Running,    // worker active
        Done,       // finished normally (ok or error captured in Result)
        Killed,     // kill() observed and the worker bailed out
    };

    static const char* stateToString(State s);

    struct Result {
        bool                ok           = false;
        std::string         output;        // human-readable answer / diff / verdict
        std::string         error;
        int                 duration_ms  = 0;
        int                 tool_calls   = 0;
        bool                budget_hit   = false;
        nlohmann::json      extra;         // role-specific blob (e.g. file list)
    };

    // Internal state shared by handle copies and the worker thread. Public
    // only because the friended factory needs to construct it.
    struct State_ {
        std::string                  id;
        std::string                  role;
        std::atomic<State>           state         { State::Pending };
        std::atomic<bool>            kill_requested{ false };
        std::chrono::steady_clock::time_point started_at;
        std::chrono::steady_clock::time_point finished_at;

        std::mutex                   mu;
        std::condition_variable      cv;             // notified on state transitions
        Result                       result;         // protected by mu

        // The worker thread is held by the State_ so the handle copies don't
        // need to track it. detach() is never called — we join in the dtor
        // when the last shared_ptr drops.
        std::thread                  worker;

        ~State_();
    };

    SpawnHandle();
    SpawnHandle(SpawnHandle&&) noexcept            = default;
    SpawnHandle& operator=(SpawnHandle&&) noexcept = default;
    SpawnHandle(const SpawnHandle&)                = default;
    SpawnHandle& operator=(const SpawnHandle&)     = default;

    bool          valid() const;
    std::string   id()    const;
    std::string   role()  const;
    State         state() const;
    bool          running()  const;
    bool          finished() const;
    int           durationMs() const;

    // Block up to `timeout` waiting for the worker to finish.
    // Returns the final state (Pending if never started, otherwise terminal).
    State         wait(std::chrono::milliseconds timeout =
                           std::chrono::milliseconds::max());

    // Cooperative cancel. Idempotent. After kill(), the worker may still run
    // for a few seconds before its next budget check fires.
    void          kill();

    // Snapshot of the result. Valid after finished() == true; partial fields
    // may be set when the worker died mid-flight.
    Result        result() const;

    // For Orchestrator (and tests) to publish the handle's state.
    std::shared_ptr<State_> state_ptr() { return p_; }
    std::shared_ptr<const State_> state_ptr() const { return p_; }

private:
    friend SpawnHandle makeSpawnHandle(std::string id, std::string role);
    explicit SpawnHandle(std::shared_ptr<State_> p);

    std::shared_ptr<State_> p_;
};

// Construct a fresh handle. The caller owns starting the worker thread —
// usually via `state_ptr()->worker = std::thread([state]{ ... });`.
SpawnHandle makeSpawnHandle(std::string id, std::string role);

// Generates a short pseudo-unique id like "rsr-7f9a3c". Not cryptographic.
std::string newSpawnId(const std::string& role_prefix);

} // namespace arise
