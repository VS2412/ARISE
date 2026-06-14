#include "cortex/orchestrator.hpp"

#include "util/log.hpp"

#include <atomic>

namespace arise {

struct Orchestrator::Impl {
    Config cfg;

    std::unique_ptr<SubAgent> curator_llm;
    std::unique_ptr<Watcher>  watcher;
    std::unique_ptr<Curator>  curator;

    std::atomic<bool> running{false};
};

Orchestrator::Orchestrator(Config cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = std::move(cfg);
    if (!p_->cfg.bb) log::error("Orchestrator: bb is required");

    if (p_->cfg.enable_watcher) {
        auto wc = p_->cfg.watcher;
        wc.bb     = p_->cfg.bb;
        wc.goals  = p_->cfg.goals;
        wc.cortex = p_->cfg.cortex;
        p_->watcher = std::make_unique<Watcher>(std::move(wc));
    }

    if (p_->cfg.enable_curator && p_->cfg.cortex) {
        auto llm_cfg = p_->cfg.curator_llm;
        if (llm_cfg.role.empty()) llm_cfg.role = "curator";
        if (llm_cfg.system_prompt.empty()) {
            llm_cfg.system_prompt =
                "You are ARISE's Curator. Reply with strict JSON only.";
        }
        llm_cfg.format_json = true;
        p_->curator_llm = std::make_unique<SubAgent>(std::move(llm_cfg));

        auto cc = p_->cfg.curator;
        cc.bb     = p_->cfg.bb;
        cc.cortex = p_->cfg.cortex;
        cc.llm    = p_->curator_llm.get();
        p_->curator = std::make_unique<Curator>(std::move(cc));
    }
}

Orchestrator::~Orchestrator() { stop(); }

bool Orchestrator::running() const { return p_->running.load(); }

Watcher*  Orchestrator::watcher()    { return p_->watcher.get(); }
Curator*  Orchestrator::curator()    { return p_->curator.get(); }
SubAgent* Orchestrator::curatorLlm() { return p_->curator_llm.get(); }

void Orchestrator::start() {
    if (!p_->cfg.bb) return;
    bool expected = false;
    if (!p_->running.compare_exchange_strong(expected, true)) return;
    if (p_->watcher) p_->watcher->start();
    if (p_->curator) p_->curator->start();
    log::info("Orchestrator: started");
}

void Orchestrator::stop() {
    if (!p_) return;
    bool was_running = p_->running.exchange(false);
    if (p_->curator) p_->curator->stop();
    if (p_->watcher) p_->watcher->stop();
    if (was_running) log::info("Orchestrator: stopped");
}

} // namespace arise
