#pragma once
#include <string>

struct SystemContext {
    std::string activeWindow;
    std::string activeApp;
    std::string clipboard;
};

class Context {
public:
    static SystemContext capture();
};