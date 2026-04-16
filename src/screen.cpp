#include "screen.hpp"
#include "logger.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <chrono>
#include <mutex>

namespace {
    std::mutex g_mu;
    std::string g_cachedKey;
    std::string g_cachedText;
    std::chrono::steady_clock::time_point g_cachedAt{};
}

void Screen::invalidateCache() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cachedKey.clear();
    g_cachedText.clear();
    g_cachedAt = {};
}

std::string Screen::capture(const std::string& cacheKey) {
    // Fast path: return cached text if the window is the same and TTL not expired.
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!cacheKey.empty() && cacheKey == g_cachedKey && !g_cachedText.empty()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - g_cachedAt).count();
            if (age < SCREEN_CACHE_TTL_SEC)
                return g_cachedText;
        }
    }

    // screenshot to temp file
    if (system("grim /tmp/aria_screen.png 2>/dev/null") != 0) return "";

    // OCR with tesseract
    if (system("tesseract /tmp/aria_screen.png /tmp/aria_screen_text -l eng quiet 2>/dev/null") != 0) {
        system("rm -f /tmp/aria_screen.png 2>/dev/null");
        return "";
    }

    std::ifstream f("/tmp/aria_screen_text.txt");
    if (!f) return "";

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();

    if (text.size() > 800) text = text.substr(0, 800) + "...";

    system("rm -f /tmp/aria_screen.png /tmp/aria_screen_text.txt 2>/dev/null");

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cachedKey  = cacheKey;
        g_cachedText = text;
        g_cachedAt   = std::chrono::steady_clock::now();
    }
    return text;
}
