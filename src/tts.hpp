#pragma once
#include <string>
#include <atomic>
#include <sys/types.h>

class TTS {
public:
    explicit TTS(const std::string& modelPath);
    void speak(const std::string& text);
    void interrupt();
    bool isSpeaking() const { return speaking_.load(); }
    bool available() const  { return available_; }

private:
    std::string modelPath_;
    bool available_{false};
    std::atomic<bool> speaking_{false};
    std::atomic<pid_t> activePid_{-1};

    void runAndTrack(const std::string& cmd);
};