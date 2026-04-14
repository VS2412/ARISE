#pragma once
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <chrono>

struct TimerEntry {
    int id;
    std::string label;
    std::chrono::steady_clock::time_point fireAt;
    bool fired = false;
};

class TimerManager {
public:
    using FireCallback = std::function<void(const std::string& label)>;

    explicit TimerManager(FireCallback onFire);
    ~TimerManager();

    int  addTimer(int durationSeconds, const std::string& label);
    bool cancel(int id);
    std::string list() const;

private:
    void runLoop();

    FireCallback onFire_;
    std::vector<TimerEntry> timers_;
    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> running_{true};
    int nextId_ = 1;
};
