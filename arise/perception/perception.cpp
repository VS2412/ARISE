#include "perception/perception.hpp"

#include "blackboard/blackboard.hpp"
#include "cortex/memory_cortex.hpp"
#include "perception/phash.hpp"
#include "perception/privacy_gate.hpp"
#include "perception/system_snapshot.hpp"
#include "util/log.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

using namespace std::chrono;
using nlohmann::json;

namespace arise {

struct Perception::Impl {
    Config cfg;

    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};

    std::thread visionT;
    std::thread systemT;
    std::thread idleT;

    std::mutex                stop_mu;
    std::condition_variable   stop_cv;

    PrivacyGate gate;

    Stats                       stats;
    mutable std::mutex          stats_mu;

    sys::Snapshot               last_snap;
    mutable std::mutex          snap_mu;

    std::atomic<std::uint64_t>  last_frame_hash{0};

    // steady_clock::now().time_since_epoch().count() of latest perceived activity.
    std::atomic<long long>      last_activity_ns{0};
    std::atomic<bool>           in_idle{false};

    explicit Impl(Config c)
        : cfg(std::move(c)),
          gate(PrivacyGate::Config{cfg.private_apps,
                                   std::chrono::milliseconds(5000),
                                   cfg.failsafe_private_on_probe_error}) {}

    bool sleepFor(int ms) {
        std::unique_lock<std::mutex> lk(stop_mu);
        return stop_cv.wait_for(lk, milliseconds(ms),
                                [this] { return stopping.load(); });
    }
};

Perception::Perception(Config cfg)
    : p_(std::make_unique<Impl>(std::move(cfg))) {
    if (!p_->cfg.bb) {
        log::error("Perception: Blackboard pointer is null — perception disabled");
    }
    // Seed activity to "now" so we don't immediately fire idle.entered.
    p_->last_activity_ns.store(steady_clock::now().time_since_epoch().count(),
                               std::memory_order_relaxed);
}

Perception::~Perception() { stop(); }

void Perception::start() {
    if (!p_->cfg.bb) return;
    bool expected = false;
    if (!p_->running.compare_exchange_strong(expected, true)) return;
    p_->stopping.store(false);

    p_->visionT = std::thread(&Perception::visionLoop, this);
    p_->systemT = std::thread(&Perception::systemLoop, this);
    p_->idleT   = std::thread(&Perception::idleLoop,   this);
    log::info("Perception: started");
}

void Perception::stop() {
    bool was_running = p_->running.exchange(false);
    if (!was_running) return;
    {
        std::lock_guard<std::mutex> lk(p_->stop_mu);
        p_->stopping.store(true);
    }
    p_->stop_cv.notify_all();
    if (p_->visionT.joinable()) p_->visionT.join();
    if (p_->systemT.joinable()) p_->systemT.join();
    if (p_->idleT.joinable())   p_->idleT.join();
    log::info("Perception: stopped");
}

bool Perception::running() const { return p_->running.load(); }

Perception::Stats Perception::stats() const {
    std::lock_guard<std::mutex> lk(p_->stats_mu);
    return p_->stats;
}

sys::Snapshot Perception::lastSnapshot() const {
    std::lock_guard<std::mutex> lk(p_->snap_mu);
    return p_->last_snap;
}

std::uint64_t Perception::lastFrameHash() const {
    return p_->last_frame_hash.load(std::memory_order_relaxed);
}

void Perception::touchActivity() {
    p_->last_activity_ns.store(steady_clock::now().time_since_epoch().count(),
                               std::memory_order_relaxed);
}

// ─── vision loop ────────────────────────────────────────────────────────────

void Perception::visionLoop() {
    if (p_->cfg.vision_interval_ms <= 0) return;

    std::uint64_t last_hash = 0;
    bool          have_last = false;

    while (!p_->stopping.load()) {
        if (p_->sleepFor(p_->cfg.vision_interval_ms)) break;

        if (p_->gate.isPrivate()) {
            { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.privacy_holds; }
            p_->cfg.bb->publish("vision.privacy_hold",
                                json{{"app", p_->gate.lastMatched()}});
            continue;
        }

        std::string cmd = "grim " + p_->cfg.grim_extra_args + " -t ppm \"" +
                          p_->cfg.screenshot_path + "\" 2>/dev/null";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.frames_failed; }
            continue;
        }

        auto h = vision::aHashFromPpm(p_->cfg.screenshot_path);
        if (!h) {
            { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.frames_failed; }
            continue;
        }

        { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.frames_captured; }
        p_->last_frame_hash.store(*h, std::memory_order_relaxed);

        if (!have_last) {
            p_->cfg.bb->publish("vision.first_frame", json{{"hash", *h}});
            last_hash = *h;
            have_last = true;
            touchActivity();
            continue;
        }

        int dist = vision::hammingDistance(last_hash, *h);
        bool changed = dist >= p_->cfg.vision_diff_threshold;

        json payload = {
            {"hash",      *h},
            {"prev_hash", last_hash},
            {"hamming",   dist},
            {"threshold", p_->cfg.vision_diff_threshold},
        };

        if (changed) {
            { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.frames_changed; }
            p_->cfg.bb->publish("vision.screen_changed", payload);
            touchActivity();
            if (p_->cfg.episodic_writes && p_->cfg.cortex) {
                EpisodicEvent ev;
                ev.kind     = "screen_obs";
                ev.summary  = "screen changed (Δhash=" + std::to_string(dist) + ")";
                ev.salience = 0.2;        // floor — captioner will boost in commit 2
                p_->cfg.cortex->recordEvent(std::move(ev));
            }
        } else {
            { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.frames_unchanged; }
            if (p_->cfg.emit_unchanged_frames)
                p_->cfg.bb->publish("vision.screen_unchanged", payload);
        }

        last_hash = *h;
    }
}

// ─── system loop ────────────────────────────────────────────────────────────

void Perception::systemLoop() {
    if (p_->cfg.system_interval_ms <= 0) return;

    sys::Snapshot prev;
    bool          first = true;

    while (!p_->stopping.load()) {
        sys::Snapshot cur = sys::take();
        { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.system_samples; }
        { std::lock_guard<std::mutex> lk(p_->snap_mu);  p_->last_snap = cur; }

        if (first) {
            p_->cfg.bb->publish("system.snapshot", sys::toJson(cur));
            first = false;
            touchActivity();
        } else {
            json d = sys::delta(prev, cur);
            if (!d.empty()) {
                { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.system_deltas; }
                p_->cfg.bb->publish("system.delta", d);
                touchActivity();
            }
        }
        prev = cur;

        if (p_->sleepFor(p_->cfg.system_interval_ms)) break;
    }
}

// ─── idle loop ──────────────────────────────────────────────────────────────

void Perception::idleLoop() {
    if (p_->cfg.idle_threshold_ms <= 0 || p_->cfg.idle_check_interval_ms <= 0) return;

    while (!p_->stopping.load()) {
        if (p_->sleepFor(p_->cfg.idle_check_interval_ms)) break;

        long long last_ns = p_->last_activity_ns.load(std::memory_order_relaxed);
        if (last_ns == 0) continue;

        auto last_tp = steady_clock::time_point(nanoseconds(last_ns));
        auto idle_for = duration_cast<milliseconds>(steady_clock::now() - last_tp).count();
        bool currently_idle = p_->in_idle.load();

        if (!currently_idle && idle_for >= p_->cfg.idle_threshold_ms) {
            p_->in_idle.store(true);
            { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.idle_entries; }
            p_->cfg.bb->publish("idle.entered",
                json{{"threshold_ms", p_->cfg.idle_threshold_ms},
                     {"idle_ms",      idle_for}});
        } else if (currently_idle && idle_for < p_->cfg.idle_threshold_ms) {
            p_->in_idle.store(false);
            { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.idle_exits; }
            p_->cfg.bb->publish("idle.left",
                json{{"idle_ms", idle_for}});
        }
    }
}

} // namespace arise
