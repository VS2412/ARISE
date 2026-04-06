#include "intent.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <string_view>

static std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(), ::tolower);
    return o;
}

AgentAction classifyIntent(const std::string& raw) {
    std::string t = lower(raw);

    // strip punctuation at end
    while (!t.empty() && (t.back() == '.' || t.back() == ',' || t.back() == '!'))
        t.pop_back();

    // --- OPEN APP ---
    static const std::vector<std::pair<std::string,std::string>> apps = {
        {"firefox",    "firefox"},
        {"browser",    "firefox"},
        {"chrome",     "chromium"},
        {"terminal",   "alacritty"},
        {"alacritty",  "alacritty"},
        {"code",       "code"},
        {"vs code",    "code"},
        {"vscode",     "code"},
        {"visual studio", "code"},
        {"spotify",    "spotify"},
        {"discord",    "discord"},
        {"telegram",   "telegram-desktop"},
        {"files",      "nautilus"},
        {"file manager","nautilus"},
        {"nautilus",   "nautilus"},
        {"calculator", "gnome-calculator"},
        {"settings",   "gnome-control-center"},
        {"steam",      "steam"},
        {"obs",        "obs"},
        {"gimp",       "gimp"},
    };
    // --- OPEN WEBSITE (intercept before app lookup) ---
    static const std::vector<std::pair<std::string,std::string>> sites = {
        {"youtube",    "https://youtube.com"},
        {"reddit",     "https://reddit.com"},
        {"github",     "https://github.com"},
        {"twitter",    "https://twitter.com"},
        {"instagram",  "https://instagram.com"},
        {"netflix",    "https://netflix.com"},
        {"gmail",      "https://mail.google.com"},
        {"google",     "https://google.com"},
        {"linkedin",   "https://linkedin.com"},
        {"wikipedia",  "https://wikipedia.org"},
        {"whatsapp",   "https://web.whatsapp.com"},
    };
    if (t.rfind("open ", 0) == 0) {
        std::string what = t.substr(5);
        for (auto& [k, v] : sites)
            if (what.find(k) != std::string::npos)
                return {"url", v};
    }
    // (existing open app block follows unchanged)
    if (t.rfind("open ", 0) == 0) {
        std::string app = t.substr(5);
        for (auto& [k, v] : apps)
            if (app.find(k) != std::string::npos)
                return {"open", v};
        return {"open", app}; // pass through verbatim
    }
    // launch / start / run X
    for (auto& prefix : {"launch ", "start ", "run "}) {
        if (t.rfind(prefix, 0) == 0) {
            std::string rest = t.substr(strlen(prefix));
            for (auto& [k, v] : apps)
                if (rest.find(k) != std::string::npos)
                    return {"open", v};
            return {"run", rest};
        }
    }

    // --- CLOSE ---
    if (t.find("close") != std::string::npos ||
        t.find("shut this") != std::string::npos ||
        t.find("kill this") != std::string::npos)
        return {"close", ""};

    // --- VOLUME ---
    if (t.find("volume up")   != std::string::npos ||
        t.find("louder")      != std::string::npos ||
        t.find("turn it up")  != std::string::npos ||
        t.find("increase volume") != std::string::npos)
        return {"volume", "up"};
    if (t.find("volume down")  != std::string::npos ||
        t.find("quieter")      != std::string::npos ||
        t.find("turn it down") != std::string::npos ||
        t.find("lower volume") != std::string::npos)
        return {"volume", "down"};
    if (t.find("mute") != std::string::npos ||
        t.find("silence") != std::string::npos)
        return {"volume", "mute"};

    // --- BRIGHTNESS ---
    if (t.find("brightness up")   != std::string::npos ||
        t.find("brighter")        != std::string::npos ||
        t.find("increase brightness") != std::string::npos)
        return {"brightness", "up"};
    if (t.find("brightness down") != std::string::npos ||
        t.find("dimmer")          != std::string::npos ||
        t.find("dim the screen")  != std::string::npos ||
        t.find("decrease brightness") != std::string::npos)
        return {"brightness", "down"};

    // --- MEDIA ---
    if (t.find("play music")  != std::string::npos ||
        t.find("play song")   != std::string::npos ||
        t.find("resume music") != std::string::npos ||
        t == "play" || t == "pause" ||
        t.find("play pause")  != std::string::npos ||
        t.find("pause music") != std::string::npos)
        return {"media", "play"};
    if (t.find("next song")   != std::string::npos ||
        t.find("next track")  != std::string::npos ||
        t == "next")
        return {"media", "next"};
    if (t.find("previous song")  != std::string::npos ||
        t.find("previous track") != std::string::npos ||
        t.find("prev song")      != std::string::npos ||
        t == "previous" || t == "prev")
        return {"media", "prev"};
    if (t == "stop music" || t == "stop playing")
        return {"media", "stop"};

    // --- SCREENSHOT ---
    if (t.find("screenshot")   != std::string::npos ||
        t.find("take a screen") != std::string::npos ||
        t.find("capture screen") != std::string::npos)
        return {"screenshot", ""};

    // --- WORKSPACE ---
    std::regex wsRe(R"(workspace (\d+)|switch to (\d+)|go to workspace (\d+)|workspace number (\d+))");
    std::smatch wsM;
    if (std::regex_search(t, wsM, wsRe)) {
        for (int i = 1; i <= 4; ++i)
            if (wsM[i].matched) return {"workspace", wsM[i].str()};
    }

    // --- TYPE TEXT ---
    static const std::string typePrefix = "type ";
    if (t.rfind(typePrefix, 0) == 0)
        return {"type", raw.substr(typePrefix.size())};

    // --- GOOGLE SEARCH ---
    std::regex googleRe(R"(google (.+)|search for (.+)|search (.+) on google)");
    std::smatch gM;
    if (std::regex_search(t, gM, googleRe)) {
        std::string q = gM[1].matched ? gM[1].str() :
                        gM[2].matched ? gM[2].str() : gM[3].str();
        // replace spaces with +
        for (char& c : q) if (c == ' ') c = '+';
        return {"url", "https://google.com/search?q=" + q};
    }

    // --- OPEN URL ---
    if (t.find("open ") != std::string::npos &&
        (t.find("http") != std::string::npos || t.find("www.") != std::string::npos)) {
        auto pos = raw.find("http");
        if (pos == std::string::npos) pos = raw.find("www.");
        return {"url", raw.substr(pos)};
    }

    // --- NOTIFY ---
    if (t.rfind("remind me ", 0) == 0)
        return {"notify", raw.substr(10)};

    // not matched — needs LLM
    return {"", ""};
}