#include "daemon.hpp"
#include "logger.hpp"
#include <csignal>
#include <atomic>
#include <cstdlib>
#include <string>
#include <iostream>

std::atomic<bool> shutdownRequested{false};
std::atomic<bool> triggerReceived{false};

void signalHandler(int) {
    shutdownRequested.store(true);
}
void triggerHandler(int) {
    triggerReceived.store(true);
}

int main() {
    // Safe HOME resolution with fallback
    const char* home = std::getenv("HOME");
    std::string logPath = home ? std::string(home) + "/.ai-agent.log"
                               : "/tmp/ai-agent.log";

    Logger::init(logPath);

    // sigaction instead of std::signal
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // for trigger
    struct sigaction sa_usr{};
    sa_usr.sa_handler = triggerHandler;
    sigemptyset(&sa_usr.sa_mask);
    sa_usr.sa_flags = 0;
    sigaction(SIGUSR1, &sa_usr, nullptr);

    Logger::info("ai-agent starting. PID: " + std::to_string(getpid()));

    Daemon daemon(shutdownRequested, triggerReceived);
    daemon.run();

    return 0;
}