// Offline tests for the perception subsystem. Exercise:
//  * the PPM aHash + Hamming distance helpers
//  * SystemSnapshot delta logic
//  * PrivacyGate matching with stub probes
//
// The Perception class itself isn't unit tested here because it requires a
// running niri/grim — that surface is covered by the `arise perceive`
// integration command in the cheat sheet.

#include "perception/phash.hpp"
#include "perception/privacy_gate.hpp"
#include "perception/system_snapshot.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace arise;
using namespace arise::vision;

namespace {

std::string makeTempPpm(const std::string& tag) {
    auto base = fs::temp_directory_path();
    auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();
    auto p = base / ("arise_phash_" + tag + "_" + std::to_string(stamp) + ".ppm");
    return p.string();
}

} // namespace

// ─── phash ─────────────────────────────────────────────────────────────────

TEST(PhashTest, HammingBasics) {
    EXPECT_EQ(hammingDistance(0ULL, 0ULL), 0);
    EXPECT_EQ(hammingDistance(~0ULL, 0ULL), 64);
    EXPECT_EQ(hammingDistance(0x1ULL, 0x3ULL), 1);
    EXPECT_EQ(hammingDistance(0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL), 64);
}

TEST(PhashTest, SameImageSameHash) {
    auto path = makeTempPpm("same");
    // Bright 64x64 square centered in a 256x256 dark canvas. With 32-pixel
    // 8x8 cells, exactly the four central cells will be bright — so their
    // average is far above the global mean (~16/255), giving a 4-bit hash.
    auto fill = [](int x, int y) -> std::array<unsigned char, 3> {
        bool bright = (x >= 96 && x < 160) && (y >= 96 && y < 160);
        unsigned char v = bright ? 255 : 0;
        return {v, v, v};
    };
    ASSERT_TRUE(writePpm(path, 256, 256, fill));

    auto h1 = aHashFromPpm(path);
    auto h2 = aHashFromPpm(path);
    ASSERT_TRUE(h1.has_value());
    ASSERT_TRUE(h2.has_value());
    EXPECT_EQ(*h1, *h2);
    EXPECT_NE(*h1, 0ULL);          // central square should set the 4 middle bits
    EXPECT_EQ(__builtin_popcountll(*h1), 4);

    fs::remove(path);
}

TEST(PhashTest, DifferentImagesDistinctHashes) {
    auto p1 = makeTempPpm("topbot");
    auto p2 = makeTempPpm("leftright");

    ASSERT_TRUE(writePpm(p1, 64, 64, [](int, int y) -> std::array<unsigned char, 3> {
        unsigned char v = (y < 32) ? 0 : 255;
        return {v, v, v};
    }));
    ASSERT_TRUE(writePpm(p2, 64, 64, [](int x, int) -> std::array<unsigned char, 3> {
        unsigned char v = (x < 32) ? 0 : 255;
        return {v, v, v};
    }));

    auto h1 = aHashFromPpm(p1);
    auto h2 = aHashFromPpm(p2);
    ASSERT_TRUE(h1 && h2);
    EXPECT_NE(*h1, *h2);
    EXPECT_GT(hammingDistance(*h1, *h2), 0);

    fs::remove(p1);
    fs::remove(p2);
}

TEST(PhashTest, BadPathReturnsNullopt) {
    EXPECT_FALSE(aHashFromPpm("/this/path/should/not/exist.ppm").has_value());
}

// ─── system snapshot delta ────────────────────────────────────────────────

TEST(SystemSnapshotTest, NoChangeYieldsEmptyDelta) {
    sys::Snapshot a, b;
    a.active_app = "kitty";
    b.active_app = "kitty";
    a.battery_pct = 80;
    b.battery_pct = 80;

    auto d = sys::delta(a, b);
    EXPECT_TRUE(d.empty());
}

TEST(SystemSnapshotTest, OnlyChangedFieldsAppearInDelta) {
    sys::Snapshot a, b;
    a.active_app   = "kitty";
    a.battery_pct  = 80;
    b.active_app   = "firefox";   // changed
    b.battery_pct  = 80;          // unchanged

    auto d = sys::delta(a, b);
    EXPECT_EQ(d.size(), 1u);
    ASSERT_TRUE(d.contains("active_app"));
    EXPECT_EQ(d["active_app"].get<std::string>(), "firefox");
    EXPECT_FALSE(d.contains("battery_pct"));
}

TEST(SystemSnapshotTest, FieldRemovalSetsNullSentinel) {
    sys::Snapshot a, b;
    a.battery_pct = 80;
    // b leaves battery_pct unset → semantically "field went away"
    auto d = sys::delta(a, b);
    ASSERT_TRUE(d.contains("battery_pct"));
    EXPECT_TRUE(d["battery_pct"].is_null());
}

TEST(SystemSnapshotTest, ToJsonSerialisesPresentFieldsOnly) {
    sys::Snapshot s;
    s.active_app = "kitty";
    s.volume_pct = 42;
    auto j = sys::toJson(s);
    EXPECT_TRUE (j.contains("active_app"));
    EXPECT_TRUE (j.contains("volume_pct"));
    EXPECT_FALSE(j.contains("battery_pct"));
    EXPECT_EQ(j["volume_pct"].get<int>(), 42);
}

// ─── privacy gate ──────────────────────────────────────────────────────────

TEST(PrivacyGateTest, WouldBlockMatchesAppList) {
    PrivacyGate::Config c;
    c.private_apps = {"keepassxc", "firefox"};
    PrivacyGate g(c);
    EXPECT_TRUE (g.wouldBlock("firefox"));
    EXPECT_TRUE (g.wouldBlock("keepassxc"));
    EXPECT_FALSE(g.wouldBlock("kitty"));
    EXPECT_FALSE(g.wouldBlock(""));
}

TEST(PrivacyGateTest, EmptyListAlwaysAllows) {
    PrivacyGate::Config c;        // no private apps
    PrivacyGate g(c);
    g.setProbe([] { return std::string("anything"); });
    EXPECT_FALSE(g.isPrivate());
}

TEST(PrivacyGateTest, ProbeStubBlocksOnMatch) {
    PrivacyGate::Config c;
    c.private_apps = {"firefox"};
    c.cache_ttl    = std::chrono::milliseconds(0);   // bust cache between checks
    PrivacyGate g(c);

    g.setProbe([] { return std::string("firefox"); });
    EXPECT_TRUE(g.isPrivate());
    EXPECT_EQ(g.lastMatched(), "firefox");

    g.setProbe([] { return std::string("kitty"); });
    EXPECT_FALSE(g.isPrivate());
    EXPECT_EQ(g.lastMatched(), "");
}

TEST(PrivacyGateTest, FailsafeFlagControlsEmptyProbeBehaviour) {
    PrivacyGate::Config c1;
    c1.private_apps = {"firefox"};
    c1.failsafe_private_on_probe_error = false;
    c1.cache_ttl = std::chrono::milliseconds(0);
    PrivacyGate g1(c1);
    g1.setProbe([] { return std::string{}; });
    EXPECT_FALSE(g1.isPrivate());

    PrivacyGate::Config c2 = c1;
    c2.failsafe_private_on_probe_error = true;
    PrivacyGate g2(c2);
    g2.setProbe([] { return std::string{}; });
    EXPECT_TRUE(g2.isPrivate());
}

TEST(PrivacyGateTest, CacheRespectsTtl) {
    PrivacyGate::Config c;
    c.private_apps = {"firefox"};
    c.cache_ttl    = std::chrono::milliseconds(200);
    PrivacyGate g(c);

    int probe_calls = 0;
    g.setProbe([&] { ++probe_calls; return std::string("firefox"); });

    EXPECT_TRUE(g.isPrivate());
    EXPECT_TRUE(g.isPrivate());
    EXPECT_TRUE(g.isPrivate());
    EXPECT_EQ(probe_calls, 1);          // cached for the whole ttl
}
