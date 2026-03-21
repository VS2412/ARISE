#pragma once
#include <string>

class Transcriber {
public:
    explicit Transcriber(const std::string& modelPath);
    ~Transcriber();

    // returns transcribed text, empty string on failure
    std::string transcribe(const std::string& wavPath);

private:
    struct whisper_context* ctx{nullptr};
};
