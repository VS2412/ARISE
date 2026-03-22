#pragma once
#include <string>

class TTS {
public:
    explicit TTS(const std::string& modelPath);
    void speak(const std::string& text);
    bool available() const { return available_; }
private:
    std::string modelPath_;
    bool available_{false};
};