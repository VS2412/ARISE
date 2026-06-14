#include "perception/perception.hpp"

#include "blackboard/blackboard.hpp"
#include "cortex/memory_cortex.hpp"
#include "cortex/salience.hpp"
#include "perception/audio_scene.hpp"
#include "perception/mic_capture.hpp"
#include "perception/phash.hpp"
#include "perception/privacy_gate.hpp"
#include "perception/system_snapshot.hpp"
#include "util/log.hpp"
#include "util/vision_client.hpp"

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

    // Caption throttle + concurrency guard.
    std::atomic<long long>      last_caption_ns{0};
    std::atomic<int>            captions_in_flight{0};

    // Audio scene state (last published bucket).
    std::mutex                  audio_mu;
    audio::Scene                last_audio_scene = audio::Scene::Unknown;
    bool                        audio_first_done = false;

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

    // Audio loop: piggybacks on MicCapture's worker thread (started below).
    if (p_->cfg.mic && p_->cfg.audio_scene && p_->cfg.audio_scene->isReady()) {
        p_->cfg.mic->setOnWindow([this](const float* samples, std::size_t n) {
            { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.audio_windows; }
            auto r = p_->cfg.audio_scene->classify(samples, n);
            audio::Scene s = r.scene;

            std::lock_guard<std::mutex> lk(p_->audio_mu);
            json payload = {
                {"scene",        audio::sceneToString(s)},
                {"score",        r.score},
                {"raw_label",    r.display_name},
                {"class_idx",    r.class_idx},
            };
            if (!p_->audio_first_done) {
                p_->cfg.bb->publish("audio.scene_first", payload);
                p_->last_audio_scene = s;
                p_->audio_first_done = true;
                return;
            }
            if (s != p_->last_audio_scene) {
                payload["prev_scene"] = audio::sceneToString(p_->last_audio_scene);
                p_->cfg.bb->publish("audio.scene_changed", payload);
                {
                    std::lock_guard<std::mutex> sk(p_->stats_mu);
                    ++p_->stats.audio_scene_changes;
                }
                if (p_->cfg.episodic_writes && p_->cfg.cortex
                    && s != audio::Scene::Unknown) {
                    EpisodicEvent ev;
                    ev.kind     = "audio_scene";
                    ev.summary  = std::string("audio: ")
                                + audio::sceneToString(p_->last_audio_scene)
                                + " → " + audio::sceneToString(s);
                    ev.payload_json = payload.dump();
                    ev.salience = (s == audio::Scene::Speech ||
                                   s == audio::Scene::Doorbell ||
                                   s == audio::Scene::Phone ||
                                   s == audio::Scene::Alarm) ? 0.5 : 0.25;
                    p_->cfg.cortex->recordEvent(std::move(ev));
                }
                p_->last_audio_scene = s;
            }
        });
        audio::MicCapture::Config mc;
        mc.alsa_device = p_->cfg.mic_device;
        if (!p_->cfg.mic->start(mc)) {
            log::warn("Perception: mic capture failed to start (mic busy?); audio off");
            p_->cfg.bb->publish("audio.error", json{{"reason", "mic_unavailable"}});
        }
    }

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
    if (p_->cfg.mic) p_->cfg.mic->stop();
    if (p_->visionT.joinable()) p_->visionT.join();
    if (p_->systemT.joinable()) p_->systemT.join();
    if (p_->idleT.joinable())   p_->idleT.join();

    // Wait briefly for in-flight caption workers to finish so they don't UAF
    // their captured `this` after the destructor returns. Bounded so a stuck
    // Ollama call can't hang shutdown indefinitely.
    auto wait_until = steady_clock::now() + seconds(5);
    while (p_->captions_in_flight.load(std::memory_order_relaxed) > 0
           && steady_clock::now() < wait_until) {
        std::this_thread::sleep_for(milliseconds(50));
    }

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

            // Try to schedule a caption. Triple gate: captioner present, no
            // worker already running (we keep concurrency at 1 to avoid Ollama
            // pile-up), and cooldown elapsed since last attempt.
            bool wrote_episodic_here = false;
            if (p_->cfg.captioner) {
                long long now_ns = steady_clock::now().time_since_epoch().count();
                long long last_ns = p_->last_caption_ns.load(std::memory_order_relaxed);
                long long elapsed_ms = (now_ns - last_ns) / 1'000'000;
                bool cool = (last_ns == 0) ||
                            elapsed_ms >= p_->cfg.caption_cooldown_ms;
                int  inflight = p_->captions_in_flight.load(std::memory_order_relaxed);
                if (cool && inflight == 0) {
                    p_->last_caption_ns.store(now_ns, std::memory_order_relaxed);
                    p_->captions_in_flight.fetch_add(1, std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lk(p_->stats_mu);
                        ++p_->stats.captions_attempted;
                    }
                    std::uint64_t fh = *h;
                    int dh = dist;
                    std::thread([this, fh, dh]() {
                        runCaptionWorker(fh, dh);
                    }).detach();
                    // Episodic write deferred to the worker so it can include
                    // the caption + real salience.
                } else {
                    {
                        std::lock_guard<std::mutex> lk(p_->stats_mu);
                        ++p_->stats.captions_throttled;
                    }
                }
            }

            // Default episodic write path (no captioner, or
            // episodic_caption_only == false).
            if (p_->cfg.episodic_writes && p_->cfg.cortex && !wrote_episodic_here) {
                bool skip_due_to_caption = p_->cfg.captioner
                                           && p_->cfg.episodic_caption_only;
                if (!skip_due_to_caption) {
                    EpisodicEvent ev;
                    ev.kind     = "screen_obs";
                    ev.summary  = "screen changed (Δhash=" + std::to_string(dist) + ")";
                    ev.salience = 0.2;
                    p_->cfg.cortex->recordEvent(std::move(ev));
                }
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

// ─── caption worker (detached thread per fired caption) ────────────────────

void Perception::runCaptionWorker(std::uint64_t frame_hash, int hamming) {
    // RAII: decrement in-flight counter even on early return / exception.
    struct Guard {
        std::atomic<int>* c;
        ~Guard() { c->fetch_sub(1, std::memory_order_relaxed); }
    } guard{ &p_->captions_in_flight };

    if (p_->stopping.load() || !p_->cfg.captioner) return;

    // Capture a fresh PNG. Re-use grim because the aHash loop wrote a P6 PPM
    // which moondream can't decode. Cheap (~50ms on a desktop) and rare
    // (gated by caption_cooldown_ms).
    std::string cmd = "grim " + p_->cfg.grim_extra_args + " "
                    + "\"" + p_->cfg.caption_image_path + "\" 2>/dev/null";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.captions_failed; }
        return;
    }

    auto caption = p_->cfg.captioner->captionFile(p_->cfg.caption_image_path);
    if (caption.empty()) {
        { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.captions_failed; }
        return;
    }

    // Salience: use the scorer if present, else a sensible mid-floor.
    SalienceScore s;
    if (p_->cfg.salience) {
        s = p_->cfg.salience->score("screen_obs", caption);
    } else {
        s.salience = 0.4;
        s.from_llm = false;
    }

    json payload = {
        {"caption",   caption},
        {"hash",      frame_hash},
        {"hamming",   hamming},
        {"salience",  s.salience},
        {"from_llm",  s.from_llm},
    };
    if (!s.reason.empty()) payload["salience_reason"] = s.reason;
    p_->cfg.bb->publish("vision.caption", payload);

    { std::lock_guard<std::mutex> lk(p_->stats_mu); ++p_->stats.captions_ok; }

    if (p_->cfg.episodic_writes && p_->cfg.cortex) {
        EpisodicEvent ev;
        ev.kind         = "screen_obs";
        ev.summary      = caption;
        ev.salience     = s.salience;
        ev.payload_json = payload.dump();
        p_->cfg.cortex->recordEvent(std::move(ev));
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
