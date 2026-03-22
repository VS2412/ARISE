#include "daemon.hpp"
#include "logger.hpp"
#include "recorder.hpp"
#include "transcriber.hpp"
#include "llm.hpp"
#include "executor.hpp"
#include "tts.hpp"
#include <thread>
#include <chrono>

Daemon::Daemon(std::atomic<bool>& flag, std::atomic<bool>& trigger)
    : shutdownRequested(flag), triggerReceived(trigger) {}

void Daemon::run() {
    Logger::info("Initializing pipeline...");

    Recorder    recorder("/tmp/ai-agent-recording.wav");
    Transcriber transcriber("/home/Aurelius/Documents/AdoVs/whisper.cpp/models/ggml-small.en.bin");
    LLM         llm("qwen2.5-coder:14b");
    Executor    executor;
    TTS         tts("/home/Aurelius/.local/share/piper/en_US-lessac-medium.onnx");

    Logger::info("ARIA ready. Waiting for trigger.");

    while (!shutdownRequested.load()) {
        if (triggerReceived.exchange(false)) {
            if (recorder.isRecording()) {
                recorder.toggle(); // stop

                std::string text = transcriber.transcribe(recorder.getOutputPath());

                if (text.empty() || text.find("[BLANK_AUDIO]") != std::string::npos) {
                    Logger::warn("No speech detected.");
                    tts.speak("I didn't catch that.");
                } else {
                    Logger::info("You said: " + text);
                    auto response = llm.think(text);

                    if (response.hasAction()) {
                        Logger::info("Action: " + response.action.type +
                                     " → " + response.action.param);
                        std::string feedback = executor.execute(response.action);
                        if (response.speech.empty() && !feedback.empty())
                            tts.speak(feedback);
                    }

                    if (!response.speech.empty())
                        tts.speak(response.speech);
                }
            } else {
                recorder.toggle(); // start
                Logger::info("Listening...");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (recorder.isRecording()) recorder.toggle();
    Logger::info("Daemon shutting down cleanly.");
}