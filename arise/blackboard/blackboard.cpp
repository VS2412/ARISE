#include "blackboard/blackboard.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

using namespace std::chrono;

namespace arise {

// ─── Subscription internals ────────────────────────────────────────────────

struct Blackboard::Subscription::Impl {
    std::string                  topic;       // "" = wildcard
    std::deque<BlackboardEvent>  queue;
    mutable std::mutex           mu;
    std::condition_variable      cv;
    bool                         stopped = false;
    std::size_t                  max_q;
    std::size_t                  dropped = 0;

    Impl(std::string t, std::size_t q) : topic(std::move(t)), max_q(q) {}

    void push(const BlackboardEvent& ev) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stopped) return;
            if (queue.size() >= max_q) {
                queue.pop_front();
                ++dropped;
            }
            queue.push_back(ev);
        }
        cv.notify_one();
    }
};

Blackboard::Subscription::Subscription(std::shared_ptr<Impl> p) : p_(std::move(p)) {}

Blackboard::Subscription::Subscription(Subscription&& o) noexcept = default;

Blackboard::Subscription&
Blackboard::Subscription::operator=(Subscription&& o) noexcept {
    if (this != &o) {
        stop();
        p_ = std::move(o.p_);
    }
    return *this;
}

Blackboard::Subscription::~Subscription() { stop(); }

void Blackboard::Subscription::stop() {
    if (!p_) return;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->stopped = true;
    }
    p_->cv.notify_all();
}

std::optional<BlackboardEvent>
Blackboard::Subscription::next(milliseconds timeout) {
    if (!p_) return std::nullopt;
    std::unique_lock<std::mutex> lk(p_->mu);
    auto ready = [&] { return p_->stopped || !p_->queue.empty(); };
    if (timeout == milliseconds::max()) {
        p_->cv.wait(lk, ready);
    } else if (!p_->cv.wait_for(lk, timeout, ready)) {
        return std::nullopt;
    }
    if (p_->queue.empty()) return std::nullopt;
    auto ev = std::move(p_->queue.front());
    p_->queue.pop_front();
    return ev;
}

std::vector<BlackboardEvent> Blackboard::Subscription::drain() {
    std::vector<BlackboardEvent> out;
    if (!p_) return out;
    std::lock_guard<std::mutex> lk(p_->mu);
    out.reserve(p_->queue.size());
    while (!p_->queue.empty()) {
        out.push_back(std::move(p_->queue.front()));
        p_->queue.pop_front();
    }
    return out;
}

std::size_t Blackboard::Subscription::pending() const {
    if (!p_) return 0;
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->queue.size();
}

std::size_t Blackboard::Subscription::dropped() const {
    if (!p_) return 0;
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->dropped;
}

const std::string& Blackboard::Subscription::topic() const {
    static const std::string empty{};
    return p_ ? p_->topic : empty;
}

// ─── Blackboard internals ──────────────────────────────────────────────────

struct Blackboard::Impl {
    std::size_t history_cap;
    std::size_t sub_q_cap;

    mutable std::mutex mu;
    std::unordered_map<std::string, std::deque<BlackboardEvent>>           history;
    std::unordered_map<std::string, std::vector<std::weak_ptr<Subscription::Impl>>> subs;
    std::vector<std::weak_ptr<Subscription::Impl>>                         wildcards;

    std::size_t total_published = 0;

    Impl(std::size_t hc, std::size_t qc) : history_cap(hc), sub_q_cap(qc) {}
};

Blackboard::Blackboard(std::size_t per_topic_history, std::size_t per_subscriber_queue)
    : p_(std::make_unique<Impl>(per_topic_history, per_subscriber_queue)) {}

Blackboard::~Blackboard() {
    std::vector<std::shared_ptr<Subscription::Impl>> alive;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& [t, v] : p_->subs)
            for (auto& w : v)
                if (auto s = w.lock()) alive.push_back(std::move(s));
        for (auto& w : p_->wildcards)
            if (auto s = w.lock()) alive.push_back(std::move(s));
    }
    for (auto& s : alive) {
        {
            std::lock_guard<std::mutex> lk(s->mu);
            s->stopped = true;
        }
        s->cv.notify_all();
    }
}

void Blackboard::publish(std::string topic, nlohmann::json payload) {
    BlackboardEvent ev{ std::move(topic), system_clock::now(), std::move(payload) };

    std::vector<std::shared_ptr<Subscription::Impl>> targets;
    {
        std::lock_guard<std::mutex> lk(p_->mu);

        auto& h = p_->history[ev.topic];
        h.push_back(ev);
        while (h.size() > p_->history_cap) h.pop_front();

        ++p_->total_published;

        auto collect = [&](std::vector<std::weak_ptr<Subscription::Impl>>& v) {
            v.erase(
                std::remove_if(v.begin(), v.end(),
                    [&](std::weak_ptr<Subscription::Impl>& w) {
                        if (auto s = w.lock()) {
                            targets.push_back(std::move(s));
                            return false;
                        }
                        return true;       // prune dead subs
                    }),
                v.end());
        };
        auto it = p_->subs.find(ev.topic);
        if (it != p_->subs.end()) collect(it->second);
        collect(p_->wildcards);
    }
    for (auto& t : targets) t->push(ev);
}

Blackboard::Subscription Blackboard::subscribe(std::string topic) {
    auto impl = std::make_shared<Subscription::Impl>(topic, p_->sub_q_cap);
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        if (impl->topic.empty()) p_->wildcards.push_back(impl);
        else                     p_->subs[impl->topic].push_back(impl);
    }
    return Subscription(std::move(impl));
}

std::vector<BlackboardEvent> Blackboard::recent(int n, std::string topic) const {
    std::vector<BlackboardEvent> out;
    if (n <= 0) return out;
    std::lock_guard<std::mutex> lk(p_->mu);

    if (!topic.empty()) {
        auto it = p_->history.find(topic);
        if (it == p_->history.end()) return out;
        const auto& h = it->second;
        int start = std::max(0, static_cast<int>(h.size()) - n);
        out.reserve(h.size() - start);
        for (int i = start; i < static_cast<int>(h.size()); ++i) out.push_back(h[i]);
        return out;
    }

    // wildcard: merge all topics, sort by ts, keep last n.
    for (auto& [t, h] : p_->history)
        for (auto& e : h) out.push_back(e);
    std::sort(out.begin(), out.end(),
              [](const BlackboardEvent& a, const BlackboardEvent& b) { return a.ts < b.ts; });
    if (static_cast<int>(out.size()) > n)
        out.erase(out.begin(), out.begin() + (out.size() - n));
    return out;
}

std::size_t Blackboard::totalPublished() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->total_published;
}

std::size_t Blackboard::subscriberCount() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::size_t n = 0;
    for (auto& [t, v] : p_->subs)
        for (auto& w : v) if (!w.expired()) ++n;
    for (auto& w : p_->wildcards) if (!w.expired()) ++n;
    return n;
}

std::vector<std::string> Blackboard::topics() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(p_->mu);
    out.reserve(p_->history.size());
    for (auto& [t, h] : p_->history) out.push_back(t);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace arise
