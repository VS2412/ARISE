#include "util/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <pwd.h>
#include <unistd.h>

namespace arise::paths {

namespace fs = std::filesystem;

std::string home() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (auto* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) return pw->pw_dir;
    return "/tmp";
}

std::string ariseRoot() {
    if (const char* r = std::getenv("ARISE_ROOT"); r && *r) return r;
    return home() + "/.arise";
}

std::string memoryDir()   { return ariseRoot() + "/memory";   }
std::string identityDir() { return ariseRoot() + "/identity"; }
std::string cacheDir()    { return ariseRoot() + "/cache";    }
std::string logsDir()     { return ariseRoot() + "/logs";     }
std::string runtimeDir()  { return ariseRoot() + "/runtime";  }
std::string goalsDbPath() { return memoryDir() + "/goals.db"; }
std::string toolsDir()         { return ariseRoot() + "/tools"; }
std::string toolsLearnedDir()  { return toolsDir() + "/learned"; }
std::string toolsSandboxDir()  { return toolsDir() + "/sandbox"; }
std::string toolsArchivedDir() { return toolsDir() + "/archived"; }
std::string feedbackDbPath()   { return memoryDir() + "/feedback.db"; }
std::string devicesJsonPath()  { return ariseRoot() + "/devices.json"; }
std::string trainingDir()      { return ariseRoot() + "/training"; }
std::string adaptersDir()      { return ariseRoot() + "/adapters"; }
std::string adaptersManifestPath() { return adaptersDir() + "/manifest.json"; }

std::string expandHome(const std::string& p) {
    if (p.size() >= 2 && p[0] == '~' && p[1] == '/') return home() + p.substr(1);
    if (p == "~") return home();
    return p;
}

bool ensureDir(const std::string& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    return !ec;
}

bool fileExists(const std::string& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

std::string ensureLayout() {
    auto r = ariseRoot();
    ensureDir(r);
    ensureDir(memoryDir());
    ensureDir(identityDir());
    ensureDir(cacheDir());
    ensureDir(logsDir());
    ensureDir(runtimeDir());
    ensureDir(toolsDir());
    ensureDir(toolsLearnedDir());
    ensureDir(toolsSandboxDir());
    ensureDir(toolsArchivedDir());
    ensureDir(trainingDir());
    ensureDir(adaptersDir());
    return r;
}

} // namespace arise::paths
