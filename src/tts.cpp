#include "tts.hpp"
#include "logger.hpp"
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

TTS::TTS(const std::string& modelPath) : modelPath_(modelPath) {
    if (system("which piper > /dev/null 2>&1") != 0) {
        Logger::warn("TTS: piper not found."); return;
    }
    if (!fs::exists(modelPath)) {
        Logger::warn("TTS: model not found: " + modelPath); return;
    }
    available_ = true;
    Logger::info("TTS: ready.");
}

void TTS::runAndTrack(const std::string& cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(1);
    }
    if (pid < 0) return;
    activePid_.store(pid);
    int status;
    waitpid(pid, &status, 0);
    activePid_.store(-1);
}

void TTS::speak(const std::string& text) {
    if (text.empty()) return;
    if (!available_) { Logger::info("TTS (silent): " + text); return; }

    std::ofstream f("/tmp/aria_tts_in.txt");
    if (!f) { Logger::error("TTS: cannot write temp file"); return; }
    f << text; f.close();

    Logger::info("TTS: speaking → " + text.substr(0, 80));
    speaking_.store(true);

    // piper renders to WAV
    runAndTrack("piper --model " + modelPath_ +
                " --output_file /tmp/aria_tts_out.wav"
                " < /tmp/aria_tts_in.txt 2>/dev/null");

    // play only if not interrupted
    if (speaking_.load())
        runAndTrack("pw-play /tmp/aria_tts_out.wav 2>/dev/null");

    speaking_.store(false);
}

void TTS::interrupt() {
    speaking_.store(false);
    pid_t pid = activePid_.load();
    if (pid > 0) {
        kill(pid, SIGTERM);
        activePid_.store(-1);
    }
    // also kill any lingering pw-play
    system("pkill -f 'pw-play /tmp/aria_tts_out.wav' 2>/dev/null");
}