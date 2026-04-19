#pragma once
#include <string>

struct SystemContext {
    std::string activeWindow;
    std::string activeApp;
    std::string clipboard;
    std::string screenText;
    std::string recentNotifications;
};

class Context {
public:
    static SystemContext capture();
};