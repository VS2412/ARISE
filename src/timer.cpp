#include "timer.hpp"
#include "logger.hpp"
#include <sstream>

TimerManager::TimerManager(FireCallback onFire)
    : onFire_(std::move(onFire)) {
    thread_ = std::thread(&TimerManager::runLoop, this);
    Logger::info("TimerManager: started.");
}

TimerManager::~TimerManager() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

int TimerManager::addTimer(int durationSeconds, const std::string& label) {
    std::lock_guard<std::mutex> lock(mutex_);
    TimerEntry t;
    t.id = nextId_++;
    t.label = label.empty() ? ("Timer " + std::to_string(t.id)) : label;
    t.fireAt = std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds);
    timers_.push_back(t);
    Logger::info("TimerManager: set #" + std::to_string(t.id) + " '" + t.label +
                 "' for " + std::to_string(durationSeconds) + "s");
    return t.id;
}

bool TimerManager::cancel(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->id == id && !it->fired) {
            Logger::info("TimerManager: cancelled #" + std::to_string(id));
            timers_.erase(it);
            return true;
        }
    }
    return false;
}

std::string TimerManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (timers_.empty()) return "No active timers.";
    auto now = std::chrono::steady_clock::now();
    std::ostringstream out;
    for (auto& t : timers_) {
        if (t.fired) continue;
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(t.fireAt - now).count();
        if (remaining < 0) remaining = 0;
        int mins = remaining / 60;
        int secs = remaining % 60;
        out << "#" << t.id << " " << t.label << ": ";
        if (mins > 0) out << mins << "m ";
        out << secs << "s remaining\n";
    }
    std::string result = out.str();
    return result.empty() ? "No active timers." : result;
}

void TimerManager::runLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::vector<TimerEntry> toFire;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto now = std::chrono::steady_clock::now();
            for (auto& t : timers_) {
                if (!t.fired && now >= t.fireAt) {
                    t.fired = true;
                    toFire.push_back(t);
                }
            }
            // Remove fired timers
            timers_.erase(
                std::remove_if(timers_.begin(), timers_.end(),
                    [](const TimerEntry& t) { return t.fired; }),
                timers_.end());
        }

        for (auto& t : toFire) {
            Logger::info("TimerManager: fired #" + std::to_string(t.id) + " '" + t.label + "'");
            if (onFire_) onFire_(t.label);
        }
    }
}
