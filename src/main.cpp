#include "daemon.hpp"
#include "logger.hpp"
#include <csignal>
#include <atomic>
#include <cstdlib>
#include <string>
#include <curl/curl.h>

std::atomic<bool> shutdownRequested{false};
std::atomic<bool> triggerReceived{false};

void signalHandler(int)  { shutdownRequested.store(true); }
void triggerHandler(int) { triggerReceived.store(true); }

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char* home = std::getenv("HOME");
    std::string logPath = home ? std::string(home) + "/.ai-agent.log" : "/tmp/ai-agent.log";
    Logger::init(logPath);

    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    struct sigaction sa_usr{};
    sa_usr.sa_handler = triggerHandler;
    sigemptyset(&sa_usr.sa_mask);
    sa_usr.sa_flags = 0;
    sigaction(SIGUSR1, &sa_usr, nullptr);

    Logger::info("ai-agent starting. PID: " + std::to_string(getpid()));

    Daemon daemon(shutdownRequested, triggerReceived);
    daemon.run();

    curl_global_cleanup();
    return 0;
}