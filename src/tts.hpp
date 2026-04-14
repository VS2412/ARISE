#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <thread>
#include <sys/types.h>

class TTS {
public:
    explicit TTS(const std::string& modelPath);
    ~TTS();

    // Batch API (still works, uses streaming internally)
    void speak(const std::string& text);

    // Streaming API — feed text chunks as they arrive from LLM
    void startStream();
    void feedChunk(const std::string& text);
    void endStream();

    void interrupt();
    bool isSpeaking() const { return speaking_.load(); }
    bool available() const  { return available_; }

private:
    std::string modelPath_;
    bool available_{false};
    std::atomic<bool> speaking_{false};
    std::atomic<pid_t> activePid_{-1};

    // Streaming internals
    std::string streamBuffer_;
    std::mutex queueMutex_;
    std::queue<std::string> sentenceQueue_;
    std::condition_variable queueCV_;
    std::thread playbackThread_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> stopPlayback_{false};

    void runAndTrack(const std::string& cmd);
    void speakSentence(const std::string& text);
    void playbackLoop();
    void flushBuffer();
};
