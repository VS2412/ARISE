#include "logger.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <mutex>
#include <iomanip>

namespace {
    std::ofstream logFile;
    std::mutex logMutex;

    std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_r(&t, &tm_buf); // thread-safe, POSIX
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    void write(const std::string& level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(logMutex);
        std::string line = "[" + level + "] [" + timestamp() + "] " + msg;
        std::cout << line << "\n";
        std::cout.flush();
        if (logFile.is_open()) {
            logFile << line << "\n";
            logFile.flush();
        }
    }
}

void Logger::init(const std::string& logfile) {
    // Close any previously open stream before reopening.
    // This is critical for test isolation: if init() is called again without
    // closing first, the old inode stays open and the new file path is not used.
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (logFile.is_open()) {
            logFile.close();
        }
        logFile.open(logfile, std::ios::app);
        if (!logFile.is_open()) {
            std::cerr << "[WARN] Could not open log file: " << logfile << "\n";
        }
    }
    // Call write outside the lock to avoid deadlocking (write() locks logMutex)
    write("INFO", "Logger initialized. Log file: " + logfile);
}

void Logger::info(const std::string& msg)  { write("INFO",  msg); }
void Logger::warn(const std::string& msg)  { write("WARN",  msg); }
void Logger::error(const std::string& msg) { write("ERROR", msg); }

// Closes the log file stream so tests can safely delete the file on disk.
void Logger::close() {
    std::lock_guard<std::mutex> lock(logMutex);
    if (logFile.is_open()) {
        logFile.close();
    }
}