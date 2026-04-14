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

TTS::~TTS() {
    interrupt();
    // Make sure playback thread is joined if still running
    if (playbackThread_.joinable()) {
        stopPlayback_.store(true);
        queueCV_.notify_all();
        playbackThread_.join();
    }
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

// Speak a single sentence: piper WAV piped through stdout to pw-play
void TTS::speakSentence(const std::string& text) {
    if (text.empty() || !available_) return;

    // Write text to temp file (avoids shell escaping issues with echo)
    std::ofstream f("/tmp/aria_tts_in.txt");
    if (!f) { Logger::error("TTS: cannot write temp file"); return; }
    f << text;
    f.close();

    Logger::info("TTS: speaking → " + text.substr(0, 80));

    // Pipe WAV from piper stdout directly into pw-play — no intermediate file
    std::string cmd =
        "piper --model " + modelPath_ +
        " --output_file /dev/stdout --quiet"
        " < /tmp/aria_tts_in.txt 2>/dev/null"
        " | pw-play - 2>/dev/null";

    runAndTrack(cmd);
}

// ─── Batch API ───

void TTS::speak(const std::string& text) {
    if (text.empty()) return;
    if (!available_) { Logger::info("TTS (silent): " + text); return; }

    speaking_.store(true);
    speakSentence(text);
    speaking_.store(false);
}

// ─── Streaming API ───

void TTS::startStream() {
    // Clean up any previous playback thread
    if (playbackThread_.joinable()) {
        stopPlayback_.store(true);
        queueCV_.notify_all();
        playbackThread_.join();
    }

    streamBuffer_.clear();
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        std::queue<std::string> empty;
        sentenceQueue_.swap(empty);
    }

    stopPlayback_.store(false);
    streaming_.store(true);
    speaking_.store(true);

    // Start the playback thread
    playbackThread_ = std::thread(&TTS::playbackLoop, this);
}

void TTS::feedChunk(const std::string& text) {
    if (!streaming_.load() || text.empty()) return;

    streamBuffer_ += text;

    // Check for sentence boundaries and enqueue complete sentences
    // Sentence ends: ". " "? " "! " or newline, or buffer > 120 chars
    while (true) {
        size_t breakPos = std::string::npos;

        // Look for sentence-ending punctuation followed by space or end
        for (size_t i = 0; i < streamBuffer_.size(); i++) {
            char c = streamBuffer_[i];
            if (c == '.' || c == '?' || c == '!') {
                // Check if followed by space, newline, or end of buffer
                if (i + 1 >= streamBuffer_.size() || streamBuffer_[i + 1] == ' ' ||
                    streamBuffer_[i + 1] == '\n') {
                    breakPos = i + 1;
                    break;
                }
            }
            if (c == '\n') {
                breakPos = i + 1;
                break;
            }
        }

        // Force break on long buffers
        if (breakPos == std::string::npos && streamBuffer_.size() > 120) {
            // Find last space to break on
            size_t spacePos = streamBuffer_.rfind(' ', 120);
            breakPos = (spacePos != std::string::npos && spacePos > 20)
                       ? spacePos + 1 : 120;
        }

        if (breakPos == std::string::npos) break;

        std::string sentence = streamBuffer_.substr(0, breakPos);
        streamBuffer_ = streamBuffer_.substr(breakPos);

        // Trim
        auto s = sentence.find_first_not_of(" \t\n\r");
        auto e = sentence.find_last_not_of(" \t\n\r");
        if (s == std::string::npos) continue;
        sentence = sentence.substr(s, e - s + 1);

        if (!sentence.empty()) {
            std::lock_guard<std::mutex> lock(queueMutex_);
            sentenceQueue_.push(sentence);
            queueCV_.notify_one();
        }
    }
}

void TTS::flushBuffer() {
    // Push whatever remains in the buffer
    auto s = streamBuffer_.find_first_not_of(" \t\n\r");
    auto e = streamBuffer_.find_last_not_of(" \t\n\r");
    if (s != std::string::npos) {
        std::string remaining = streamBuffer_.substr(s, e - s + 1);
        if (!remaining.empty()) {
            std::lock_guard<std::mutex> lock(queueMutex_);
            sentenceQueue_.push(remaining);
            queueCV_.notify_one();
        }
    }
    streamBuffer_.clear();
}

void TTS::endStream() {
    if (!streaming_.load()) return;

    // Flush remaining buffer as final sentence
    flushBuffer();

    // Signal end of stream
    streaming_.store(false);
    queueCV_.notify_all();

    // Wait for playback thread to finish
    if (playbackThread_.joinable())
        playbackThread_.join();

    speaking_.store(false);
}

void TTS::playbackLoop() {
    while (true) {
        std::string sentence;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait(lock, [this] {
                return !sentenceQueue_.empty() || !streaming_.load() || stopPlayback_.load();
            });

            if (stopPlayback_.load()) {
                // Drain queue and exit
                std::queue<std::string> empty;
                sentenceQueue_.swap(empty);
                return;
            }

            if (sentenceQueue_.empty()) {
                // Stream ended and queue is drained
                if (!streaming_.load()) return;
                continue;
            }

            sentence = sentenceQueue_.front();
            sentenceQueue_.pop();
        }

        if (stopPlayback_.load()) return;

        speakSentence(sentence);

        if (stopPlayback_.load()) return;
    }
}

void TTS::interrupt() {
    streaming_.store(false);
    stopPlayback_.store(true);
    speaking_.store(false);

    // Kill active piper/pw-play process
    pid_t pid = activePid_.load();
    if (pid > 0) {
        kill(pid, SIGTERM);
        activePid_.store(-1);
    }

    // Kill any lingering processes
    system("pkill -f 'pw-play.*/dev/null' 2>/dev/null");
    system("pkill -f 'piper.*output_file' 2>/dev/null");

    // Drain the sentence queue
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        std::queue<std::string> empty;
        sentenceQueue_.swap(empty);
    }
    queueCV_.notify_all();

    // Wait for playback thread to finish
    if (playbackThread_.joinable())
        playbackThread_.join();
}
