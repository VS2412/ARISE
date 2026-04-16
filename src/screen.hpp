#pragma once
#include <string>

class Screen {
public:
    // Returns OCR text of current screen.
    // Result is cached per focusedApp+window for SCREEN_CACHE_TTL_SEC seconds
    // to avoid re-running tesseract when context hasn't meaningfully changed.
    // Pass different cacheKey to invalidate (e.g. activeApp|activeWindow).
    static std::string capture(const std::string& cacheKey = "");

    // Force-clear the cache (useful after known display changes).
    static void invalidateCache();

    static constexpr int SCREEN_CACHE_TTL_SEC = 60;
};
