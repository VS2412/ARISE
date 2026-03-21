#include "daemon.hpp"
#include "logger.hpp"
#include "recorder.hpp"
#include "transcriber.hpp"
#include <thread>
#include <chrono>
#include <string>
#include <cstdlib>

Daemon::Daemon(std::atomic<bool>& flag, std::atomic<bool>& trigger)
    : shutdownRequested(flag), triggerReceived(trigger) {}

void Daemon::run() {
    Logger::info("Daemon started. Entering event loop.");

    Recorder    recorder("/tmp/ai-agent-recording.wav");
    const char* modelEnv = std::getenv("AI_AGENT_MODEL");
    std::string modelPath = modelEnv ? modelEnv : "/home/Aurelius/Documents/AdoVs/whisper.cpp/models/ggml-base.en.bin";
    Transcriber transcriber(modelPath);

    while (!shutdownRequested.load()) {
        if (triggerReceived.exchange(false)) {
            if (recorder.isRecording()) {
                recorder.toggle();  // stop

                const std::string wavPath = recorder.getOutputPath();
                Logger::info("Transcribing file: " + wavPath);
                auto t0 = std::chrono::steady_clock::now();
                std::string text = transcriber.transcribe(recorder.getOutputPath());
                auto t1 = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                Logger::info("Transcribe finished in " + std::to_string(ms) + " ms.");

                if (!text.empty()) {
                    Logger::info("Transcription: " + text);
                    // Phase 4: pass text to LLM here
                } else {
                    Logger::warn("Transcription returned empty.");
                }
            } else {
                recorder.toggle();  // start
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (recorder.isRecording()) recorder.toggle();
    Logger::info("Daemon shutting down cleanly.");
}