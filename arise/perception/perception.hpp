#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perception/system_snapshot.hpp"

namespace arise {

class Blackboard;
class MemoryCortex;
class PrivacyGate;
class VisionClient;
class SalienceScorer;

namespace audio {
class SceneClassifier;
class MicCapture;
} // namespace audio

// Always-on perception loop. Spawns up to four internal threads:
//   * vision  — grim → PPM → 8x8 aHash → diff → blackboard event
//                       └→ on change + cooldown ok: grim PNG → moondream caption
//   * system  — poll niri/wpctl/brightnessctl/sysfs every N ms, post deltas
//   * idle    — track time since last "activity" event; emit idle in/out
//   * audio   — opt-in arecord → YAMNet scene classifier → transitions
//
// Topics published:
//   "vision.first_frame"      — first hash captured
//   "vision.screen_changed"   — hamming ≥ threshold vs prior
//   "vision.screen_unchanged" — hamming < threshold (low-volume, suppressed by default)
//   "vision.privacy_hold"     — capture skipped because active app is private
//   "vision.caption"          — moondream caption (async, may arrive after
//                               several screen_changed events have fired)
//   "system.snapshot"         — first full snapshot at start
//   "system.delta"            — fields that changed since last poll
//   "idle.entered"            — no activity for idle_threshold_ms
//   "idle.left"               — activity resumed; payload includes idle_ms
//   "audio.scene_first"       — first classified window
//   "audio.scene_changed"     — coarse scene transition (silence→speech, …)
//   "audio.error"             — mic capture stopped unexpectedly
class Perception {
public:
    struct Config {
        Blackboard*    bb     = nullptr;     // required
        MemoryCortex*  cortex = nullptr;     // optional — episodic writes

        // Captioner + salience scorer (optional). Both must be set for
        // captioning to fire; salience-less captions still publish a caption
        // event but use the default 0.4 floor for episodic salience.
        VisionClient*    captioner = nullptr;
        SalienceScorer*  salience  = nullptr;

        // Audio scene classifier + mic. Both required for the audio loop.
        audio::SceneClassifier* audio_scene = nullptr;
        audio::MicCapture*      mic         = nullptr;
        std::string             mic_device  = "default";

        std::string    screenshot_path     = "/tmp/arise_frame.ppm";
        std::string    caption_image_path  = "/tmp/arise_frame.png";
        std::string    grim_extra_args;       // e.g. "-o DP-1" to limit output

        int  vision_interval_ms     = 1000;   // 1 fps default
        int  system_interval_ms     = 10000;  // 10 s
        int  idle_check_interval_ms = 5000;
        int  idle_threshold_ms      = 60000;  // 60 s of no events = idle
        int  vision_diff_threshold  = 8;      // hamming bits to count as changed
        int  caption_cooldown_ms    = 5000;   // min ms between caption calls
        bool emit_unchanged_frames  = false;  // set true to flood-test recall
        bool episodic_writes        = true;   // log changed frames to episodic
        bool episodic_caption_only  = true;   // when captioner present, only
                                              // write episodic for captioned frames

        std::vector<std::string> private_apps;
        bool failsafe_private_on_probe_error = false;
    };

    explicit Perception(Config cfg);
    ~Perception();
    Perception(const Perception&)            = delete;
    Perception& operator=(const Perception&) = delete;

    void start();
    void stop();
    bool running() const;

    struct Stats {
        std::size_t frames_captured     = 0;
        std::size_t frames_changed      = 0;
        std::size_t frames_unchanged    = 0;
        std::size_t frames_failed       = 0;
        std::size_t system_samples      = 0;
        std::size_t system_deltas       = 0;
        std::size_t privacy_holds       = 0;
        std::size_t idle_entries        = 0;
        std::size_t idle_exits          = 0;
        std::size_t captions_attempted  = 0;
        std::size_t captions_ok         = 0;
        std::size_t captions_failed     = 0;
        std::size_t captions_throttled  = 0;
        std::size_t audio_windows       = 0;
        std::size_t audio_scene_changes = 0;
    };
    Stats          stats()        const;
    sys::Snapshot  lastSnapshot() const;

    // For tests / CLI inspection: latest computed aHash, 0 if none yet.
    std::uint64_t  lastFrameHash() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;

    void visionLoop();
    void systemLoop();
    void idleLoop();
    void touchActivity();

    // Triggered from visionLoop on screen_changed; runs caption + salience
    // synchronously inside the spawned worker, publishes vision.caption,
    // and writes the episodic event. Detached so it doesn't stall capture.
    void runCaptionWorker(std::uint64_t frame_hash, int hamming);
};

} // namespace arise
