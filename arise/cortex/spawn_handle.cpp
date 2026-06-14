#include "cortex/spawn_handle.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>

namespace arise {

const char* SpawnHandle::stateToString(State s) {
    switch (s) {
        case State::Pending: return "pending";
        case State::Running: return "running";
        case State::Done:    return "done";
        case State::Killed:  return "killed";
    }
    return "pending";
}

SpawnHandle::State_::~State_() {
    if (worker.joinable()) {
        // Last-resort: ask the worker to bail and join.
        kill_requested.store(true);
        worker.join();
    }
}

SpawnHandle::SpawnHandle()                                = default;
SpawnHandle::SpawnHandle(std::shared_ptr<State_> p) : p_(std::move(p)) {}

bool        SpawnHandle::valid() const { return p_ != nullptr; }
std::string SpawnHandle::id()    const { return p_ ? p_->id : ""; }
std::string SpawnHandle::role()  const { return p_ ? p_->role : ""; }

SpawnHandle::State SpawnHandle::state() const {
    return p_ ? p_->state.load() : State::Pending;
}

bool SpawnHandle::running()  const { return state() == State::Running; }
bool SpawnHandle::finished() const {
    auto s = state();
    return s == State::Done || s == State::Killed;
}

int SpawnHandle::durationMs() const {
    if (!p_) return 0;
    auto end = p_->finished_at;
    if (end.time_since_epoch().count() == 0) end = std::chrono::steady_clock::now();
    auto begin = p_->started_at;
    if (begin.time_since_epoch().count() == 0) return 0;
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
}

SpawnHandle::State SpawnHandle::wait(std::chrono::milliseconds timeout) {
    if (!p_) return State::Pending;
    std::unique_lock<std::mutex> lk(p_->mu);
    p_->cv.wait_for(lk, timeout, [&]{ return finished(); });
    return p_->state.load();
}

void SpawnHandle::kill() {
    if (!p_) return;
    p_->kill_requested.store(true);
    p_->cv.notify_all();
}

SpawnHandle::Result SpawnHandle::result() const {
    if (!p_) return {};
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->result;
}

SpawnHandle makeSpawnHandle(std::string id, std::string role) {
    auto state = std::make_shared<SpawnHandle::State_>();
    state->id   = std::move(id);
    state->role = std::move(role);
    return SpawnHandle(std::move(state));
}

std::string newSpawnId(const std::string& role_prefix) {
    static thread_local std::mt19937_64 rng(
        std::random_device{}() ^ std::uint64_t(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    char buf[24];
    std::snprintf(buf, sizeof buf, "%s-%06lx",
                  role_prefix.c_str(),
                  static_cast<unsigned long>(rng() & 0xFFFFFF));
    return buf;
}

} // namespace arise
