#include "intent.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <vector>

static std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(), ::tolower);
    return o;
}

static const std::vector<std::string> kKnownApps = {
    "firefox","browser","chrome","chromium","terminal","alacritty","kitty",
    "code","vscode","vs code","visual studio","spotify","discord","telegram",
    "files","file manager","nautilus","calculator","settings","steam","obs",
    "gimp","vlc","thunar","dolphin","blender","inkscape"
};

static bool isKnownApp(const std::string& name) {
    auto t = lower(name);
    for (auto& a : kKnownApps)
        if (t.find(a) != std::string::npos) return true;
    return false;
}

static std::string trimPunct(std::string s) {
    while (!s.empty() && (s.back()=='.'||s.back()==','||s.back()=='!'||s.back()=='?'))
        s.pop_back();
    return s;
}

AgentAction classifyIntent(const std::string& raw) {
    std::string t = lower(trimPunct(raw));

    // WEBSITE SHORTCUTS — instant, no LLM
    static const std::vector<std::pair<std::string,std::string>> kSites = {
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

    // Detect compound commands — "open X and do Y" should go to LLM
    auto hasCompound = [](const std::string& s) -> bool {
        for (auto& sep : {" and ", " then ", " and then ", " also "})
            if (s.find(sep) != std::string::npos) return true;
        return false;
    };

    // OPEN — check websites first, then apps, else fall through to LLM
    auto tryOpen = [&](const std::string& rest) -> AgentAction {
        // If compound command, let LLM handle the whole thing
        if (hasCompound(rest)) return {};
        for (auto& [k, v] : kSites)
            if (rest.find(k) != std::string::npos)
                return {"url", {{"url", v}}};
        if (isKnownApp(rest)) return {"open", {{"app", rest}}};
        return {}; // unknown — let LLM handle
    };

    for (auto& prefix : {"open ", "launch ", "start ", "go to "}) {
        if (t.rfind(prefix, 0) == 0)
            return tryOpen(t.substr(strlen(prefix)));
    }
    // "can you open X" / "please open X"
    {
        std::regex openRe(R"((?:can you|please|could you|aria)\s+open\s+(.+))");
        std::smatch m;
        if (std::regex_search(t, m, openRe))
            return tryOpen(m[1].str());
    }

    // CLOSE
    if (t.find("close") != std::string::npos ||
        t.find("kill this") != std::string::npos ||
        t.find("shut this") != std::string::npos)
        return {"close", {}};

    // ARCH PACKAGE MANAGEMENT
    if (t.find("install ") != std::string::npos) {
        std::regex pkgRe(R"(install\s+(\S+))");
        std::smatch m;
        if (std::regex_search(t, m, pkgRe))
            return {"run", {{"command", "alacritty -e sudo pacman -S " + m[1].str()}}};
    }
    if (t.find("remove ") != std::string::npos ||
        t.find("uninstall ") != std::string::npos) {
        std::regex pkgRe(R"((?:remove|uninstall)\s+(\S+))");
        std::smatch m;
        if (std::regex_search(t, m, pkgRe))
            return {"run", {{"command", "alacritty -e sudo pacman -R " + m[1].str()}}};
    }

    // VOLUME
    if (t.find("volume up")   != std::string::npos ||
        t.find("turn up")     != std::string::npos ||
        t.find("louder")      != std::string::npos ||
        t.find("increase volume") != std::string::npos)  return {"volume", {{"action","up"}}};
    if (t.find("volume down") != std::string::npos ||
        t.find("turn down")   != std::string::npos ||
        t.find("quieter")     != std::string::npos ||
        t.find("lower volume")!= std::string::npos)      return {"volume", {{"action","down"}}};
    if (t.find("mute") != std::string::npos)             return {"volume", {{"action","mute"}}};

    // BRIGHTNESS
    if (t.find("brighter")   != std::string::npos ||
        t.find("brightness up") != std::string::npos)    return {"brightness", {{"action","up"}}};
    if (t.find("dimmer")     != std::string::npos ||
        t.find("brightness down") != std::string::npos ||
        t.find("dim the") != std::string::npos)          return {"brightness", {{"action","down"}}};

    // MEDIA
    if (t == "play" || t == "pause" ||
        t.find("play music")  != std::string::npos ||
        t.find("play song")   != std::string::npos ||
        t.find("resume music")!= std::string::npos ||
        t.find("play pause")  != std::string::npos)      return {"media", {{"action","play"}}};
    if (t == "next" || t.find("next song") != std::string::npos ||
        t.find("next track")  != std::string::npos)      return {"media", {{"action","next"}}};
    if (t == "previous" || t == "prev" ||
        t.find("previous song")  != std::string::npos ||
        t.find("previous track") != std::string::npos)   return {"media", {{"action","prev"}}};
    if (t.find("stop music")   != std::string::npos ||
        t.find("stop playing") != std::string::npos)     return {"media", {{"action","stop"}}};

    // SCREENSHOT
    if (t.find("screenshot")    != std::string::npos ||
        t.find("capture screen")!= std::string::npos ||
        t.find("take a screen") != std::string::npos)    return {"screenshot", {}};

    // WORKSPACE
    {
        std::regex wsRe(R"(workspace\s*(\d+)|switch\s+to\s+(\d+)|go\s+to\s+workspace\s+(\d+))");
        std::smatch m;
        if (std::regex_search(t, m, wsRe))
            for (int i=1;i<=3;i++)
                if (m[i].matched) return {"workspace", {{"number", m[i].str()}}};
    }

    // TYPE
    if (t.rfind("type ", 0) == 0)
        return {"type", {{"text", raw.substr(5)}}};

    // WEB SEARCH — only explicit web-search phrasing.
    // Bare "search X" falls through to LLM so it can pick file_search vs web_search.
    {
        std::regex gRe(
            R"((?:^|\s)(?:google(?:\s+for)?|search\s+(?:the\s+web|online|the\s+internet|google|wikipedia|ddg|duckduckgo)(?:\s+for)?|web\s+search(?:\s+for)?|look\s+up)\s+(.+))"
        );
        std::smatch m;
        if (std::regex_search(t, m, gRe)) {
            std::string query = m[1].str();
            for (char& c : query) if (c == ' ') c = '+';
            return {"url", {{"url", "https://google.com/search?q=" + query}}};
        }
    }

    // URL
    if (t.find("http") != std::string::npos || t.find("www.") != std::string::npos) {
        auto pos = raw.find("http");
        if (pos == std::string::npos) pos = raw.find("www.");
        return {"url", {{"url", raw.substr(pos)}}};
    }

    // CLIPBOARD
    if (t.find("what's in my clipboard") != std::string::npos ||
        t.find("read clipboard")         != std::string::npos ||
        t.find("what did i copy")        != std::string::npos)
        return {"clipboard", {{"action", "read"}}};

    // SYSTEM UPDATE — requires "system", "arch", or "pacman" alongside "update"
    if (t.find("update") != std::string::npos &&
        (t.find("system") != std::string::npos ||
         t.find("arch")   != std::string::npos ||
         t.find("pacman") != std::string::npos))
        return {"run", {{"command", "alacritty -e sudo pacman -Syu"}}};

    return {}; // LLM handles everything else
}
