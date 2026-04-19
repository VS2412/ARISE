#pragma once
#include <atomic>
#include <chrono>
#include <string>

#include "llm.hpp"  // for LLMResponse, LLMContext, AgentAction

class Transcriber;
class Executor;
class TTS;
class Memory;
class TimerManager;
class Recorder;

class Daemon {
public:
    explicit Daemon(std::atomic<bool>& shutdownFlag,
                    std::atomic<bool>& pauseToggle);
    void run();

private:
    // Extracted from the old monolithic run() lambda for readability.
    void        processUtterance(const std::string& rawText);
    void        handleLLMResponse(LLMResponse& response, LLMContext& ctx,
                                  const std::string& origText);
    std::string executeAction(const AgentAction& act);
    void        maybeSummarize();

    std::atomic<bool>& shutdownRequested;
    std::atomic<bool>& pauseToggle;

    // Subsystems — stack-allocated in run(); member pointers set so the
    // extracted methods can reach them without captures. Valid only while
    // run() is on the stack.
    Transcriber*  transcriber_ = nullptr;
    LLM*          llm_         = nullptr;
    Executor*     executor_    = nullptr;
    TTS*          tts_         = nullptr;
    Memory*       memory_      = nullptr;
    TimerManager* timers_      = nullptr;
    Recorder*     recorder_    = nullptr;

    // Processing state
    std::atomic<bool> ariaActive_{true};
    std::string       lastFactKey_;
    std::chrono::steady_clock::time_point lastFactTime_{};
    bool              wakeRequired_ = false;
    std::chrono::steady_clock::time_point awakeUntil_{};

    static constexpr auto kFollowUpWindow = std::chrono::seconds(5);
};
