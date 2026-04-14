#include "screen.hpp"
#include "logger.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>

std::string Screen::capture() {
    // screenshot to temp file
    system("grim /tmp/aria_screen.png 2>/dev/null");

    // OCR with tesseract
    system("tesseract /tmp/aria_screen.png /tmp/aria_screen_text -l eng quiet 2>/dev/null");

    // read result
    std::ifstream f("/tmp/aria_screen_text.txt");
    if (!f) return "";

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();

    // truncate to 800 chars — enough for context, not token-wasteful
    if (text.size() > 800) text = text.substr(0, 800) + "...";

    // clean up
    system("rm -f /tmp/aria_screen.png /tmp/aria_screen_text.txt");
    return text;
}
