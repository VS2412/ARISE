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
std::string goalsDbPath();   // <root>/memory/goals.db
std::string toolsDir();      // <root>/tools
std::string toolsLearnedDir();   // <root>/tools/learned
std::string toolsSandboxDir();   // <root>/tools/sandbox
std::string toolsArchivedDir();  // <root>/tools/archived
std::string feedbackDbPath();    // <root>/memory/feedback.db
std::string devicesJsonPath();   // <root>/devices.json
std::string trainingDir();       // <root>/training
std::string adaptersDir();       // <root>/adapters
std::string adaptersManifestPath();  // <root>/adapters/manifest.json

// Idempotently mkdir -p the full layout. Returns the resolved root.
std::string ensureLayout();

std::string expandHome(const std::string& p);
bool        ensureDir (const std::string& p);
bool        fileExists(const std::string& p);

} // namespace arise::paths
