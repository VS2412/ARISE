#pragma once
#include <string>

namespace Logger {
    // Opens (or reopens) the log file at the given path.
    // Closes any previously open stream before reopening, so this is safe
    // to call multiple times (e.g. between unit tests).
    void init(const std::string& logfile);

    void info(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);

    // Explicitly closes the log file stream.
    // Required by the test fixture to fully release the file handle
    // before TearDown() removes the temporary file from disk.
    void close();
}