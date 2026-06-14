#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace arise::audio {

// Opt-in always-on mic capture. Wraps `arecord` as a child process and
// streams 16-bit PCM out of its stdout into a rolling float32 ring buffer.
// Whenever the buffer accumulates `hop_samples` new samples, we emit a
// `window_samples`-long window via the registered callback.
//
// Why arecord (not libasound directly)?
//   - Decouples ARISE from libasound entirely (no extra link dep)
//   - Cleanly fails when the mic is busy (e.g. ARIA daemon holding it),
//     instead of competing for an exclusive ALSA handle
//   - Trivial to test (point at a file with `--alsa-device file:...`)
//
// Capture is best-effort. If arecord exits or fails to start, `running()`
// flips false and the consumer should treat the audio scene as Unknown.
class MicCapture {
public:
    using WindowCb = std::function<void(const float* samples,
                                        std::size_t n)>;

    struct Config {
        std::string alsa_device   = "default";
        int         sample_rate   = 16000;
        std::size_t window_samples = 15600;   // ≈ 0.975s @ 16kHz (YAMNet frame)
        std::size_t hop_samples    = 8000;    // emit ~2 windows/sec
    };

    MicCapture();
    ~MicCapture();
    MicCapture(const MicCapture&)            = delete;
    MicCapture& operator=(const MicCapture&) = delete;

    void setOnWindow(WindowCb cb);

    bool start(const Config& cfg);
    void stop();
    bool running() const;

    // Bytes successfully read from arecord since start(). Useful for tests
    // and "is the mic actually producing data" debugging.
    std::size_t bytesRead() const;
    std::size_t windowsEmitted() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace arise::audio
