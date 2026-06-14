#pragma once

#include <memory>

#include "cortex/curator.hpp"
#include "cortex/sub_agent.hpp"
#include "cortex/watcher.hpp"

namespace arise {

class Blackboard;
class GoalStore;
class MemoryCortex;

// Phase 4 commit 1 facade. Owns the long-running sub-agents (Watcher,
// Curator) plus a shared SubAgent LLM that they can invoke. start()/stop()
// drive their lifecycles together.
//
// Commit 2 grows this into the real orchestrator: Researcher / Coder /
// Critic, the spawn(role, task, budget) → handle API, blackboard event
// routing, budget enforcement, sub-agent kill-switch.
class Orchestrator {
public:
    struct Config {
        Blackboard*    bb     = nullptr;     // required
        GoalStore*     goals  = nullptr;     // optional — Watcher proposes goals if set
        MemoryCortex*  cortex = nullptr;     // optional — fact sink for Curator + episodic mirrors

        SubAgent::Config curator_llm;        // qwen3:0.6b CPU by default
        Watcher::Config  watcher;            // bb / goals / cortex auto-filled at start()
        Curator::Config  curator;            // same — auto-filled

        bool enable_watcher = true;
        bool enable_curator = true;
    };

    explicit Orchestrator(Config cfg);
    ~Orchestrator();
    Orchestrator(const Orchestrator&)            = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

    void start();
    void stop();
    bool running() const;

    Watcher* watcher();        // nullptr if disabled
    Curator* curator();        // nullptr if disabled
    SubAgent* curatorLlm();    // nullptr if disabled

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace arise
