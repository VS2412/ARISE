#pragma once
#include <string>
#include <memory>
#include <cstdint>
#include <cstddef>

// Audio wake-word detector using openWakeWord ONNX models.
//
// Pipeline (per 80ms chunk of 16kHz int16 PCM):
//   audio → melspectrogram.onnx → embedding_model.onnx → <wake>.onnx → prob
//
// Trigger uses an activation counter that increments on frames above
// `threshold` and decays otherwise. Fires when the counter hits
// `triggerLevel`, then cools down for `refractoryFrames`.
//
// Thread-safety: single-threaded. Instantiate on the audio capture thread.
class WakeWord {
public:
    struct Config {
        std::string melspecModelPath;      // melspectrogram.onnx
        std::string embeddingModelPath;    // embedding_model.onnx
        std::string wakewordModelPath;     // e.g. hey_jarvis_v0.1.onnx
        float       threshold        = 0.5f;
        int         triggerLevel     = 4;   // consecutive hot frames to fire
        int         refractoryFrames = 20;  // cooldown (frames ≈ 80ms each)
        bool        debug            = false;
    };

    WakeWord();
    ~WakeWord();

    // Non-copyable, non-movable (owns ORT state).
    WakeWord(const WakeWord&)            = delete;
    WakeWord& operator=(const WakeWord&) = delete;

    // Load models. Returns true on success; logs and returns false on failure.
    bool init(const Config& cfg);
    bool isReady() const;

    // Feed exactly 1280 samples (80ms @ 16kHz). Returns true if the trigger
    // condition fired on this chunk. The raw probability is always stored in
    // lastProbability().
    bool processChunk(const int16_t* samples, size_t n);

    float lastProbability() const { return lastProb_; }

    // Clear internal pipeline buffers and activation state. Call after a wake
    // trigger to avoid immediate re-fires on the tail of the same audio.
    void reset();

    static constexpr int kChunkSamples = 1280;  // 80ms @ 16kHz

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    float lastProb_ = 0.0f;
};
