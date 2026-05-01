#pragma once
#include <string>

// Tiny, mutex-guarded logger for ARISE. Writes to stderr always, plus an
// optional rolling file. Independent of ARIA's Logger so the cortex can be
// linked into tests without dragging the daemon's globals along.
namespace arise::log {

enum class Level { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void init(const std::string& file_path = "");   // empty = stderr only
void setLevel(Level lvl);

void info (const std::string& s);
void warn (const std::string& s);
void error(const std::string& s);
void debug(const std::string& s);

} // namespace arise::log
