#include "util/log.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace arise::log {

namespace {

std::mutex        g_mu;
std::ofstream     g_file;
std::atomic<int>  g_level{static_cast<int>(Level::Info)};

const char* tag(Level l) {
    switch (l) {
        case Level::Error: return "ERR ";
        case Level::Warn:  return "WARN";
        case Level::Info:  return "INFO";
        case Level::Debug: return "DBG ";
    }
    return "????";
}

void emit(Level lvl, const std::string& s) {
    if (static_cast<int>(lvl) > g_level.load(std::memory_order_relaxed)) return;
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    std::strftime(ts, sizeof ts, "%F %T", &tm_buf);

    std::lock_guard<std::mutex> lk(g_mu);
    char line[1024];
    std::snprintf(line, sizeof line, "[%s] %s arise: %s\n",
                  ts, tag(lvl), s.c_str());
    std::fputs(line, stderr);
    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }
}

} // namespace

void init(const std::string& file_path) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file.is_open()) g_file.close();
    if (!file_path.empty()) g_file.open(file_path, std::ios::app);
}

void setLevel(Level lvl) {
    g_level.store(static_cast<int>(lvl), std::memory_order_relaxed);
}

void info (const std::string& s) { emit(Level::Info,  s); }
void warn (const std::string& s) { emit(Level::Warn,  s); }
void error(const std::string& s) { emit(Level::Error, s); }
void debug(const std::string& s) { emit(Level::Debug, s); }

} // namespace arise::log
