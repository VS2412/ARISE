#pragma once
#include <string>

namespace Logger {
    void init(const std::string& logfile);
    void info(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);
}