#include "daemon.hpp"
#include "logger.hpp"
#include <csignal>
#include <atomic>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <curl/curl.h>

std::atomic<bool> shutdownRequested{false};
std::atomic<bool> pauseToggle{false};

void shutdownHandler(int) { shutdownRequested.store(true); }
void pauseHandler(int)    { pauseToggle.store(true); }

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char* home = std::getenv("HOME");
    std::string logPath = home ? std::string(home) + "/.ai-agent.log" : "/tmp/ai-agent.log";
    Logger::init(logPath);

    struct sigaction sa{};
    sa.sa_handler = shutdownHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    struct sigaction sa2{};
    sa2.sa_handler = pauseHandler;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = 0;
    sigaction(SIGUSR1, &sa2, nullptr);

    Logger::info("ai-agent starting. PID: " + std::to_string(getpid()));

    Daemon daemon(shutdownRequested, pauseToggle);
    daemon.run();

    curl_global_cleanup();
    return 0;
}