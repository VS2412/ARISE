#pragma once
#include <atomic>

class Daemon {
public:
    explicit Daemon(std::atomic<bool>& shutdownFlag,
                    std::atomic<bool>& pauseToggle);
    void run();
private:
    std::atomic<bool>& shutdownRequested;
    std::atomic<bool>& pauseToggle;
};