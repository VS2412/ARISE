#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace arise {

using BBTimestamp = std::chrono::system_clock::time_point;

// One event on the Blackboard. Payload is a structured JSON blob — every
// component agrees on a topic-specific schema by convention rather than enum
// to keep the bus open to in-flight extension.
struct BlackboardEvent {
    std::string    topic;
    BBTimestamp    ts;
    nlohmann::json payload;
};

// Thread-safe pub/sub bus. Pull-based: subscribers hold a handle and call
// next() on their own thread. Publish fans out into per-subscriber bounded
// queues; if a subscriber falls behind, oldest events are dropped (counted
// in dropped()).  A global per-topic history ring backs recent() so cold
// readers can see what just happened without subscribing first.
//
// Subscriptions auto-deregister via weak_ptr when their handle is destroyed.
// Blackboard's destructor wakes any blocked next() calls; surviving handles
// can still drain() what they already buffered.
class Blackboard {
public:
    explicit Blackboard(std::size_t per_topic_history     = 256,
                        std::size_t per_subscriber_queue  = 1024);
    ~Blackboard();
    Blackboard(const Blackboard&)            = delete;
    Blackboard& operator=(const Blackboard&) = delete;

    // Publish to a topic. ts is set to system_clock::now(). Returns immediately
    // after enqueueing into subscribers (which may drop on overflow).
    void publish(std::string topic, nlohmann::json payload);

    // Move-only RAII subscription handle.
    class Subscription {
    public:
        Subscription() = default;
        ~Subscription();
        Subscription(Subscription&&) noexcept;
        Subscription& operator=(Subscription&&) noexcept;
        Subscription(const Subscription&)            = delete;
        Subscription& operator=(const Subscription&) = delete;

        // Block up to `timeout` waiting for the next event. Returns nullopt
        // on stop or timeout. With the default infinite timeout, only stop()
        // (or Blackboard destruction) can return nullopt.
        std::optional<BlackboardEvent> next(
            std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

        // Wake any blocked next() and refuse future blocking calls. drain()
        // still returns any buffered events.
        void stop();

        // Pull all currently buffered events, non-blocking.
        std::vector<BlackboardEvent> drain();

        std::size_t pending() const;
        std::size_t dropped() const;     // number of events evicted by overflow
        const std::string& topic() const;
        bool valid() const { return p_ != nullptr; }

    private:
        friend class Blackboard;
        struct Impl;
        std::shared_ptr<Impl> p_;
        explicit Subscription(std::shared_ptr<Impl> p);
    };

    // topic == "" → wildcard, receives every published event.
    Subscription subscribe(std::string topic = "");

    // Latest n events (newest last). topic == "" merges all topics.
    std::vector<BlackboardEvent> recent(int n = 50, std::string topic = "") const;

    // Stats
    std::size_t totalPublished() const;
    std::size_t subscriberCount() const;
    std::vector<std::string> topics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace arise
