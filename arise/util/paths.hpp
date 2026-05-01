#pragma once
#include <string>

// Path resolution for ARISE's on-disk layout under ~/.arise (or $ARISE_ROOT).
// Every other module reads from these helpers — never hard-codes paths.
namespace arise::paths {

std::string home();          // $HOME or pwd entry, /tmp on total failure
std::string ariseRoot();     // $ARISE_ROOT or <home>/.arise

std::string memoryDir();     // <root>/memory
std::string identityDir();   // <root>/identity
std::string cacheDir();      // <root>/cache
std::string logsDir();       // <root>/logs
std::string runtimeDir();    // <root>/runtime

// Idempotently mkdir -p the full layout. Returns the resolved root.
std::string ensureLayout();

std::string expandHome(const std::string& p);
bool        ensureDir (const std::string& p);
bool        fileExists(const std::string& p);

} // namespace arise::paths
