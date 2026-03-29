#include "daemon.hpp"
#include "logger.hpp"
#include "recorder.hpp"
#include "transcriber.hpp"
#include "llm.hpp"
#include "intent.hpp"
#include "executor.hpp"
#include "tts.hpp"
#include "context.hpp"
#include "memory.hpp"
#include <thread>
#include <chrono>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdlib>

Daemon::Daemon(std::atomic<bool>& flag, std::atomic<bool>& pause)
    : shutdownRequested(flag), pauseToggle(pause) {}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool isInterruptCommand(const std::string& text) {
    auto t = lower(text);
    return t.find("stop")     != std::string::npos ||
           t.find("cancel")   != std::string::npos ||
           t.find("shut up")  != std::string::npos ||
           t.find("quiet")    != std::string::npos ||
           t.find("enough")   != std::string::npos;
}

// extract name if user says "my name is X" or "I am X"
static std::string extractName(const std::string& text) {
    auto t = lower(text);
    auto tryExtract = [&](const std::string& prefix) -> std::string {
        auto pos = t.find(prefix);
        if (pos == std::string::npos) return "";
        std::string rest = text.substr(pos + prefix.size());
        auto end = rest.find_first_of(".,!?");
        if (end != std::string::npos) rest = rest.substr(0, end);
        while (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
        return rest;
    };
    auto name = tryExtract("my name is ");
    if (name.empty()) name = tryExtract("i am ");
    if (name.empty()) name = tryExtract("call me ");
    return name;
}

void Daemon::run() {
    Logger::info("Initializing pipeline...");

    const char* home = std::getenv("HOME");
    std::string dbPath = home ? std::string(home) + "/.aria_memory.db" : "/tmp/aria_memory.db";

    Transcriber transcriber("/home/Aurelius/Documents/AdoVs/whisper.cpp/models/ggml-small.en.bin");
    LLM         llm("aria-agent");
    Executor    executor;
    TTS         tts("/home/Aurelius/.local/share/piper/en_US-lessac-medium.onnx");
    Memory      memory(dbPath);

    std::mutex              qMutex;
    std::queue<std::string> speechQueue;
    std::condition_variable qCV;
    std::atomic<bool>       ariaActive{true};

    Recorder recorder("/tmp/aria_speech.wav",
        [&](const std::string& wavPath) {
            std::lock_guard<std::mutex> lock(qMutex);
            speechQueue.push(wavPath);
            qCV.notify_one();
        });

    std::thread processor([&]() {
    while (!shutdownRequested.load()) {
        std::unique_lock<std::mutex> lock(qMutex);
        qCV.wait_for(lock, std::chrono::milliseconds(100),
            [&]{ return !speechQueue.empty() || shutdownRequested.load(); });
        if (speechQueue.empty()) continue;
        std::string wavPath = speechQueue.front();
        speechQueue.pop();
        lock.unlock();

        if (!ariaActive.load()) continue;

        std::string text = transcriber.transcribe(wavPath);
        if (text.empty() || text.find("[BLANK_AUDIO]") != std::string::npos) {
            Logger::info("VAD: blank, skipping.");
            continue;
        }
        Logger::info("You said: " + text);

        if (isInterruptCommand(text)) {
            tts.interrupt();
            Logger::info("ARIA: interrupted.");
            continue;
        }

        // learn name passively
        auto name = extractName(text);
        if (!name.empty()) {
            memory.setFact("user_name", name);
            Logger::info("Memory: learned name = " + name);
        }

        // INTENT CLASSIFIER FIRST — no LLM needed for known commands
        AgentAction directAction = classifyIntent(text);

        recorder.mute();

        if (!directAction.type.empty()) {
            // known command — execute instantly
            Logger::info("Intent: " + directAction.type + " → " + directAction.param);
            std::string feedback = executor.execute(directAction);
            memory.save("user", text);
            memory.save("assistant", directAction.type + ":" + directAction.param);
            if (!feedback.empty()) tts.speak(feedback);
        } else {
            // unknown — send to LLM for reasoning
            auto sysCtx = Context::capture();
            LLMContext ctx;
            ctx.activeApp     = sysCtx.activeApp;
            ctx.activeWindow  = sysCtx.activeWindow;
            ctx.clipboard     = sysCtx.clipboard;
            ctx.memorySummary = memory.getSummary();

            auto userName = memory.getFact("user_name");
            std::string prompt = text;
            if (!userName.empty() &&
                (lower(text).find("hello") != std::string::npos ||
                 lower(text).find("hey")   != std::string::npos))
                prompt = text + " (user's name is " + userName + ")";

            auto response = llm.think(prompt, ctx);

            if (response.hasAction()) {
                Logger::info("LLM action: " + response.action.type +
                             " → " + response.action.param);
                std::string feedback = executor.execute(response.action);
                memory.save("user", text);
                memory.save("assistant", response.action.type + ":" + response.action.param);
                if (response.speech.empty() && !feedback.empty())
                    tts.speak(feedback);
            }
            if (!response.speech.empty()) {
                memory.save("user", text);
                memory.save("assistant", response.speech);
                tts.speak(response.speech);
            }
        }

        recorder.unmute();
    }
});

    recorder.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // startup greeting with name if known
    recorder.mute();
    auto userName = memory.getFact("user_name");
    tts.speak(userName.empty() ? "Online." : "Online, " + userName + ".");
    recorder.unmute();

    Logger::info("ARIA ready.");

    while (!shutdownRequested.load()) {
        if (pauseToggle.exchange(false)) {
            bool nowActive = !ariaActive.load();
            ariaActive.store(nowActive);
            recorder.mute();
            tts.speak(nowActive ? "Back." : "Pausing.");
            if (nowActive) recorder.unmute();
            Logger::info(nowActive ? "ARIA: resumed." : "ARIA: paused.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    recorder.stop();
    processor.join();
    Logger::info("Daemon shutting down cleanly.");
}