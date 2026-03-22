#include "tts.hpp"
#include "logger.hpp"
#include <fstream>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

TTS::TTS(const std::string& modelPath) : modelPath_(modelPath) {
    if (system("which piper > /dev/null 2>&1") != 0) {
        Logger::warn("TTS: piper binary not found. Voice output disabled.");
        return;
    }
    if (!fs::exists(modelPath)) {
        Logger::warn("TTS: model not found at " + modelPath + ". Voice output disabled.");
        return;
    }
    available_ = true;
    Logger::info("TTS: ready.");
}

void TTS::speak(const std::string& text) {
    if (text.empty()) return;
    if (!available_) {
        Logger::info("TTS (silent): " + text);
        return;
    }

    // write to file to avoid shell injection
    std::ofstream f("/tmp/aria_tts_in.txt");
    if (!f) { Logger::error("TTS: cannot write temp file"); return; }
    f << text;
    f.close();

    Logger::info("TTS: speaking → " + text);
    std::string cmd = "piper --model " + modelPath_ +
                      " --output_file /tmp/aria_tts_out.wav"
                      " < /tmp/aria_tts_in.txt 2>/dev/null"
                      " && pw-play /tmp/aria_tts_out.wav 2>/dev/null";
    system(cmd.c_str());
}