#include "cortex/piper_engine.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace std::chrono;

namespace arise {

namespace {

bool fileIsExecutable(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

std::string searchPath(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        return fileIsExecutable(name) ? name : std::string{};
    }
    const char* p = ::getenv("PATH");
    if (!p) return {};
    std::string s = p;
    std::size_t i = 0;
    while (i <= s.size()) {
        auto j = s.find(':', i);
        std::string dir = s.substr(i, (j == std::string::npos ? s.size() : j) - i);
        if (dir.empty()) dir = ".";
        auto cand = dir + "/" + name;
        if (fileIsExecutable(cand)) return cand;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return {};
}

bool fileReadable(const std::string& path) {
    if (path.empty()) return false;
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string fmtDouble(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

void killAndReap(pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 30; ++i) {
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0) return;
        ::usleep(10 * 1000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
}

} // namespace

PiperEngine::PiperEngine(Config cfg) : cfg_(std::move(cfg)) {}

bool PiperEngine::isAvailable() const {
    if (searchPath(cfg_.piper_bin).empty()) return false;
    if (cfg_.play_audio && searchPath(cfg_.aplay_bin).empty()) return false;
    if (!cfg_.default_model_path.empty()
        && !fileReadable(cfg_.default_model_path)) return false;
    return true;
}

PiperEngine::Result PiperEngine::speak(std::string_view text,
                                       const TtsParams& params) {
    Result r;
    if (text.empty()) { r.error = "piper: empty text"; return r; }

    auto piper = searchPath(cfg_.piper_bin);
    if (piper.empty()) { r.error = "piper: binary not on PATH"; return r; }

    std::string aplay;
    if (cfg_.play_audio) {
        aplay = searchPath(cfg_.aplay_bin);
        if (aplay.empty()) { r.error = "piper: aplay not on PATH"; return r; }
    }

    auto model = params.voice.empty() ? cfg_.default_model_path : params.voice;
    if (model.empty() || !fileReadable(model)) {
        r.error = "piper: model path missing or unreadable";
        return r;
    }

    // Build piper argv. We use --output_raw so stdout becomes a continuous
    // PCM stream we either play or drop.
    std::vector<std::string> piper_argv = {
        piper,
        "--model",            model,
        "--output_raw",
        "--length_scale",     fmtDouble(params.length_scale),
        "--noise_scale",      fmtDouble(params.noise_scale),
        "--noise_w",          fmtDouble(params.noise_w),
        "--sentence_silence", fmtDouble(params.sentence_silence_sec),
    };

    int piper_in[2]  = {-1, -1};
    int piper_out[2] = {-1, -1};
    if (::pipe(piper_in) != 0 || ::pipe(piper_out) != 0) {
        r.error = std::string("piper: pipe(): ") + std::strerror(errno);
        for (int* p : {piper_in, piper_out}) {
            if (p[0] >= 0) ::close(p[0]);
            if (p[1] >= 0) ::close(p[1]);
        }
        return r;
    }

    auto build_cargv = [](const std::vector<std::string>& argv,
                          std::vector<std::vector<char>>& bufs,
                          std::vector<char*>& cargv) {
        for (const auto& s : argv) {
            bufs.emplace_back(s.begin(), s.end());
            bufs.back().push_back('\0');
        }
        for (auto& b : bufs) cargv.push_back(b.data());
        cargv.push_back(nullptr);
    };

    std::vector<std::vector<char>> piper_bufs;
    std::vector<char*>             piper_cargv;
    build_cargv(piper_argv, piper_bufs, piper_cargv);

    // Spawn piper.
    posix_spawn_file_actions_t pfa;
    posix_spawn_file_actions_init(&pfa);
    posix_spawn_file_actions_adddup2(&pfa, piper_in[0],  STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&pfa, piper_out[1], STDOUT_FILENO);
    // Always silence piper's stderr — its spdlog info chatter leaks through
    // every call. Our wall-clock + byte counts are all we need; piper's
    // own real-time factor logs aren't worth the noise.
    posix_spawn_file_actions_addopen(&pfa, STDERR_FILENO,
                                     "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&pfa, piper_in[1]);
    posix_spawn_file_actions_addclose(&pfa, piper_out[0]);

    auto start = steady_clock::now();
    pid_t piper_pid = -1;
    int rc = ::posix_spawn(&piper_pid, piper.c_str(), &pfa, nullptr,
                           piper_cargv.data(), environ);
    posix_spawn_file_actions_destroy(&pfa);

    ::close(piper_in[0]);
    ::close(piper_out[1]);

    if (rc != 0) {
        r.error = std::string("piper: posix_spawn: ") + std::strerror(rc);
        ::close(piper_in[1]);
        ::close(piper_out[0]);
        return r;
    }

    // Spawn aplay (or replace piper_out[0] read with /dev/null discard).
    pid_t aplay_pid = -1;
    int   sink_fd   = -1;
    if (cfg_.play_audio) {
        std::vector<std::string> aplay_argv = {
            aplay, "-q",
            "-r", std::to_string(cfg_.sample_rate),
            "-f", "S16_LE",
            "-t", "raw", "-",
        };
        std::vector<std::vector<char>> abufs;
        std::vector<char*>             acargv;
        build_cargv(aplay_argv, abufs, acargv);

        posix_spawn_file_actions_t afa;
        posix_spawn_file_actions_init(&afa);
        posix_spawn_file_actions_adddup2(&afa, piper_out[0], STDIN_FILENO);
        posix_spawn_file_actions_addopen(&afa, STDERR_FILENO,
                                         "/dev/null", O_WRONLY, 0);
        rc = ::posix_spawn(&aplay_pid, aplay.c_str(), &afa, nullptr,
                           acargv.data(), environ);
        posix_spawn_file_actions_destroy(&afa);
        if (rc != 0) {
            r.error = std::string("piper: aplay spawn: ") + std::strerror(rc);
            ::close(piper_in[1]);
            ::close(piper_out[0]);
            killAndReap(piper_pid);
            return r;
        }
        ::close(piper_out[0]);   // child has it now
    } else {
        sink_fd = piper_out[0];   // we'll drain it ourselves
    }

    // Push the sentence to piper stdin.
    {
        const char* data = text.data();
        std::size_t need = text.size();
        std::size_t sent = 0;
        while (sent < need) {
            ssize_t w = ::write(piper_in[1], data + sent, need - sent);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;
            }
            sent += w;
        }
        ::close(piper_in[1]);
    }

    // Drain output if we kept it (no aplay).
    std::size_t bytes = 0;
    if (sink_fd >= 0) {
        char buf[4096];
        auto deadline = steady_clock::now() + cfg_.timeout;
        while (true) {
            if (steady_clock::now() >= deadline) break;
            ssize_t n = ::read(sink_fd, buf, sizeof buf);
            if (n > 0) { bytes += std::size_t(n); continue; }
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        }
        ::close(sink_fd);
    }

    // Wait for piper.
    {
        int status = 0;
        auto deadline = steady_clock::now() + cfg_.timeout;
        while (true) {
            pid_t got = ::waitpid(piper_pid, &status, WNOHANG);
            if (got == piper_pid) break;
            if (got < 0) break;
            if (steady_clock::now() >= deadline) {
                killAndReap(piper_pid);
                r.error = "piper: timed out";
                break;
            }
            ::usleep(20 * 1000);
        }
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) != 0 && r.error.empty()) {
                r.error = "piper exited with code " +
                          std::to_string(WEXITSTATUS(status));
            }
        }
    }

    // Wait for aplay.
    if (aplay_pid > 0) {
        int status = 0;
        auto deadline = steady_clock::now() + cfg_.timeout;
        while (true) {
            pid_t got = ::waitpid(aplay_pid, &status, WNOHANG);
            if (got == aplay_pid) break;
            if (got < 0) break;
            if (steady_clock::now() >= deadline) {
                killAndReap(aplay_pid);
                if (r.error.empty()) r.error = "aplay: timed out";
                break;
            }
            ::usleep(20 * 1000);
        }
    }

    auto end = steady_clock::now();
    r.duration_ms       = int(duration_cast<milliseconds>(end - start).count());
    r.bytes_synthesized = bytes;
    r.ok                = r.error.empty();
    return r;
}

} // namespace arise
