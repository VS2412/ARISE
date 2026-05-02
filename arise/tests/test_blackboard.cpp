// Round-trip + invariant tests for the Blackboard event bus.
//
// All tests run with bounded buffers so we exercise overflow paths and the
// stop/destructor wake-up logic without any background threads of our own
// outside the explicit cases.

#include "blackboard/blackboard.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace arise;
using nlohmann::json;
using namespace std::chrono_literals;

TEST(BlackboardTest, PublishSubscribeRoundTrip) {
    Blackboard bb;
    auto sub = bb.subscribe("topic.a");
    bb.publish("topic.a", json{{"x", 1}});

    auto ev = sub.next(100ms);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->topic, "topic.a");
    EXPECT_EQ(ev->payload["x"].get<int>(), 1);
}

TEST(BlackboardTest, PerTopicRouting) {
    Blackboard bb;
    auto a = bb.subscribe("topic.a");
    auto b = bb.subscribe("topic.b");

    bb.publish("topic.a", json::object());
    bb.publish("topic.b", json::object());

    EXPECT_TRUE (a.next(100ms).has_value());
    EXPECT_FALSE(a.next(50ms).has_value());        // no second event for "a"
    EXPECT_TRUE (b.next(100ms).has_value());
}

TEST(BlackboardTest, WildcardCatchAll) {
    Blackboard bb;
    auto w = bb.subscribe("");
    bb.publish("a", json::object());
    bb.publish("b", json::object());
    bb.publish("c", json::object());

    auto evs = w.drain();
    EXPECT_EQ(evs.size(), 3u);
    EXPECT_EQ(evs[0].topic, "a");
    EXPECT_EQ(evs[1].topic, "b");
    EXPECT_EQ(evs[2].topic, "c");
}

TEST(BlackboardTest, RecentReturnsHistoryWithoutSubscriber) {
    Blackboard bb(/*history*/ 10);
    for (int i = 0; i < 3; ++i) bb.publish("a", json{{"i", i}});

    auto rec = bb.recent(10, "a");
    ASSERT_EQ(rec.size(), 3u);
    EXPECT_EQ(rec.back().payload["i"].get<int>(), 2);
}

TEST(BlackboardTest, RecentRingBufferEvictsOldest) {
    Blackboard bb(/*history*/ 3);
    for (int i = 0; i < 10; ++i) bb.publish("a", json{{"i", i}});

    auto rec = bb.recent(10, "a");
    ASSERT_EQ(rec.size(), 3u);
    EXPECT_EQ(rec.front().payload["i"].get<int>(), 7);
    EXPECT_EQ(rec.back ().payload["i"].get<int>(), 9);
}

TEST(BlackboardTest, RecentWildcardSortsByTimestamp) {
    Blackboard bb(/*history*/ 10);
    bb.publish("a", json{{"i", 1}});
    std::this_thread::sleep_for(2ms);
    bb.publish("b", json{{"i", 2}});
    std::this_thread::sleep_for(2ms);
    bb.publish("a", json{{"i", 3}});

    auto rec = bb.recent(10, "");
    ASSERT_EQ(rec.size(), 3u);
    EXPECT_EQ(rec[0].payload["i"].get<int>(), 1);
    EXPECT_EQ(rec[1].payload["i"].get<int>(), 2);
    EXPECT_EQ(rec[2].payload["i"].get<int>(), 3);
}

TEST(BlackboardTest, SubscriberQueueOverflowDropsOldest) {
    Blackboard bb(/*history*/ 256, /*sub_q*/ 3);
    auto sub = bb.subscribe("a");

    for (int i = 0; i < 5; ++i) bb.publish("a", json{{"i", i}});

    EXPECT_EQ(sub.dropped(), 2u);
    auto evs = sub.drain();
    ASSERT_EQ(evs.size(), 3u);
    EXPECT_EQ(evs.front().payload["i"].get<int>(), 2);
    EXPECT_EQ(evs.back ().payload["i"].get<int>(), 4);
}

TEST(BlackboardTest, MultipleSubscribersFanout) {
    Blackboard bb;
    auto a = bb.subscribe("topic");
    auto b = bb.subscribe("topic");
    auto w = bb.subscribe("");

    bb.publish("topic", json{{"x", 7}});
    EXPECT_TRUE(a.next(100ms).has_value());
    EXPECT_TRUE(b.next(100ms).has_value());
    EXPECT_TRUE(w.next(100ms).has_value());
}

TEST(BlackboardTest, StopUnblocksNext) {
    Blackboard bb;
    auto sub = bb.subscribe("a");

    std::atomic<bool> returned_empty{false};
    std::thread t([&] {
        auto ev = sub.next();    // infinite block
        returned_empty.store(!ev.has_value());
    });
    std::this_thread::sleep_for(50ms);
    sub.stop();
    t.join();
    EXPECT_TRUE(returned_empty.load());
}

TEST(BlackboardTest, BlackboardDestructorWakesBlockedSubscribers) {
    auto bb = std::make_unique<Blackboard>();
    Blackboard::Subscription sub = bb->subscribe("a");

    std::atomic<bool> returned_empty{false};
    std::thread t([&] {
        auto ev = sub.next();
        returned_empty.store(!ev.has_value());
    });
    std::this_thread::sleep_for(50ms);
    bb.reset();              // destroying blackboard while sub is blocked
    t.join();
    EXPECT_TRUE(returned_empty.load());
}

TEST(BlackboardTest, DeadSubscriberPrunedOnNextPublish) {
    Blackboard bb;
    {
        auto s1 = bb.subscribe("topic");
        EXPECT_GE(bb.subscriberCount(), 1u);
    }
    // s1 dropped; the publish below sweeps the weak_ptr list.
    bb.publish("topic", json::object());
    EXPECT_EQ(bb.subscriberCount(), 0u);
}

TEST(BlackboardTest, TopicsLists) {
    Blackboard bb;
    bb.publish("zeta",  json::object());
    bb.publish("alpha", json::object());
    bb.publish("alpha", json::object());

    auto ts = bb.topics();
    ASSERT_EQ(ts.size(), 2u);
    EXPECT_EQ(ts[0], "alpha");
    EXPECT_EQ(ts[1], "zeta");
    EXPECT_EQ(bb.totalPublished(), 3u);
}
