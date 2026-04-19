#pragma once
#include <string>

class Transcriber {
public:
    explicit Transcriber(const std::string& modelPath);
    ~Transcriber();

    // returns transcribed text, empty string on failure
    std::string transcribe(const std::string& wavPath);

    // Counter for consecutive whisper_full failures. Daemon uses this to
    // announce mic trouble after the threshold is crossed.
    int  consecutiveFailures() const { return consecutiveFailures_; }
    void resetFailures() { consecutiveFailures_ = 0; }

private:
    struct whisper_context* ctx{nullptr};
    int consecutiveFailures_ = 0;
};
