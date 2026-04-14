#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <deque>

class Recorder {
public:
    using SpeechCallback = std::function<void(const std::string&)>;

    explicit Recorder(const std::string& outputPath, SpeechCallback onSpeech);
    ~Recorder();

    void start();
    void stop();
    void mute();
    void unmute();
    bool isActive() const { return active_.load(); }

private:
    void captureLoop();
    void writeWav(const std::vector<int16_t>& samples);

    std::string       outputPath_;
    SpeechCallback    onSpeech_;
    std::atomic<bool> active_{false};
    std::atomic<bool> muted_{false};
    std::thread       thread_;

    static constexpr int RATE         = 16000;
    static constexpr int FRAME_MS     = 20;
    static constexpr int FRAME_SAMPLES = RATE * FRAME_MS / 1000; // 320
    static constexpr int ONSET_FRAMES  = 10;   // 200ms of voice to start
    static constexpr int TRAIL_FRAMES  = 25;  // 500ms of silence to stop
    static constexpr int MIN_FRAMES    = 10;  // 200ms minimum utterance
    static constexpr int MAX_FRAMES    = 1000; // 20s hard cap
    static constexpr int PREROLL       = 5;   // frames before onset
};