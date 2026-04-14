#pragma once
#include <string>

struct SystemContext {
    std::string activeWindow;
    std::string activeApp;
    std::string clipboard;
    std::string screenText;
};

class Context {
public:
    static SystemContext capture();
};