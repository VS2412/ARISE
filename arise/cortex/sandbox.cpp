#include "cortex/sandbox.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace arise {

namespace {

bool fileIsExecutable(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

// Walk PATH for a bare name. Returns full path or empty.
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
        auto candidate = dir + "/" + name;
        if (fileIsExecutable(candidate)) return candidate;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return {};
}

// Best-effort drain on a non-blocking fd until EAGAIN / EOF / cap reached.
void drainFd(int fd, std::string& dst, std::size_t cap, bool& truncated) {
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            std::size_t take = std::size_t(n);
            if (dst.size() + take > cap) {
                if (dst.size() < cap) dst.append(buf, cap - dst.size());
                truncated = true;
                return;
            }
            dst.append(buf, take);
            continue;
        }
        if (n == 0) return;          // EOF
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        return;                       // other errors: treat as EOF
    }
}

void killProcess(pid_t pid) {
    if (pid <= 0) return;
    // bwrap also runs --die-with-parent, but be defensive.
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0) return;
        ::usleep(10 * 1000);
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

} // namespace

Sandbox::Sandbox(Config cfg) : cfg_(std::move(cfg)) {}

bool Sandbox::isAvailable(const std::string& bwrap_path) {
    auto p = searchPath(bwrap_path);
    return !p.empty();
}

Sandbox::Result Sandbox::run(const std::vector<std::string>& argv,
                             const std::string& stdin_text,
                             std::atomic<bool>* kill) const {
    Result r;
    if (argv.empty()) {
        r.error = "sandbox: empty argv";
        return r;
    }
    if (kill && kill->load()) {
        r.killed = true; r.error = "killed before spawn"; return r;
    }
    auto bwrap = searchPath(cfg_.bwrap_path);
    if (bwrap.empty()) {
        r.error = "sandbox: bwrap unavailable (looked for '" + cfg_.bwrap_path + "')";
        return r;
    }

    // Compose the bwrap command. bwrap reads its options from argv until it
    // sees an option that takes the path-to-execute; we pass the full
    // user-supplied argv after the isolation flags.
    std::vector<std::string> cmd;
    cmd.reserve(argv.size() + 32);
    cmd.push_back(bwrap);
    cmd.push_back("--die-with-parent");
    cmd.push_back("--unshare-all");
    if (cfg_.allow_network) {
        cmd.push_back("--share-net");
    }
    cmd.push_back("--ro-bind"); cmd.push_back("/"); cmd.push_back("/");
    cmd.push_back("--tmpfs");   cmd.push_back("/tmp");
    cmd.push_back("--proc");    cmd.push_back("/proc");
    cmd.push_back("--dev");     cmd.push_back("/dev");
    cmd.push_back("--new-session");
    for (const auto& w : cfg_.writable_paths) {
        if (w.empty()) continue;
        cmd.push_back("--bind-try"); cmd.push_back(w); cmd.push_back(w);
    }
    for (const auto& ro : cfg_.readonly_paths) {
        if (ro.empty()) continue;
        cmd.push_back("--ro-bind-try"); cmd.push_back(ro); cmd.push_back(ro);
    }
    if (!cfg_.chdir_in_sandbox.empty()) {
        cmd.push_back("--chdir"); cmd.push_back(cfg_.chdir_in_sandbox);
    }
    // Default: no env. If the caller passed env strings, set them via --setenv.
    cmd.push_back("--clearenv");
    for (const auto& kv : cfg_.env) {
        auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        cmd.push_back("--setenv");
        cmd.push_back(kv.substr(0, eq));
        cmd.push_back(kv.substr(eq + 1));
    }

    cmd.push_back("--");
    for (const auto& a : argv) cmd.push_back(a);

    // Build argv array (null-terminated, mutable c-strings).
    std::vector<std::vector<char>> bufs;
    bufs.reserve(cmd.size());
    for (const auto& s : cmd) {
        bufs.emplace_back(s.begin(), s.end());
        bufs.back().push_back('\0');
    }
    std::vector<char*> child_argv;
    child_argv.reserve(bufs.size() + 1);
    for (auto& b : bufs) child_argv.push_back(b.data());
    child_argv.push_back(nullptr);

    // Pipes: parent reads stdout/stderr, writes stdin.
    int stdin_pipe[2]  = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        r.error = std::string("sandbox: pipe(): ") + std::strerror(errno);
        for (int* p : {stdin_pipe, stdout_pipe, stderr_pipe}) {
            if (p[0] >= 0) ::close(p[0]);
            if (p[1] >= 0) ::close(p[1]);
        }
        return r;
    }

    // Set parent-side reads non-blocking for our poll loop.
    ::fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, stdin_pipe[0],  STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, stderr_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, stdin_pipe[1]);
    posix_spawn_file_actions_addclose(&fa, stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, stderr_pipe[0]);

    auto start = std::chrono::steady_clock::now();
    pid_t pid = -1;
    int rc = ::posix_spawn(&pid, bwrap.c_str(), &fa, nullptr,
                           child_argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);

    // Parent doesn't read its own stdin write-end / writes to stdout/err read-ends.
    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    if (rc != 0) {
        r.error = std::string("sandbox: posix_spawn: ") + std::strerror(rc);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        return r;
    }

    // Push stdin and close.
    if (!stdin_text.empty()) {
        ssize_t total = 0;
        while (total < ssize_t(stdin_text.size())) {
            ssize_t w = ::write(stdin_pipe[1],
                                stdin_text.data() + total,
                                stdin_text.size() - total);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;
            }
            total += w;
        }
    }
    ::close(stdin_pipe[1]);

    auto deadline = start + cfg_.timeout;
    bool out_open = true, err_open = true;
    while (out_open || err_open) {
        // External kill?
        if (kill && kill->load()) {
            r.killed = true;
            killProcess(pid);
            pid = -1;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            r.timed_out = true;
            killProcess(pid);
            pid = -1;
            break;
        }
        struct pollfd pfds[2];
        int nfds = 0;
        if (out_open) { pfds[nfds].fd = stdout_pipe[0]; pfds[nfds].events = POLLIN; ++nfds; }
        if (err_open) { pfds[nfds].fd = stderr_pipe[0]; pfds[nfds].events = POLLIN; ++nfds; }
        int p_rc = ::poll(pfds, nfds, 100);
        if (p_rc < 0) {
            if (errno == EINTR) continue;
            r.error = std::string("sandbox: poll(): ") + std::strerror(errno);
            killProcess(pid);
            pid = -1;
            break;
        }
        for (int i = 0; i < nfds; ++i) {
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (pfds[i].fd == stdout_pipe[0]) {
                    drainFd(stdout_pipe[0], r.stdout_text,
                            cfg_.max_stdout_bytes, r.stdout_truncated);
                    if (pfds[i].revents & (POLLHUP | POLLERR)) out_open = false;
                } else {
                    drainFd(stderr_pipe[0], r.stderr_text,
                            cfg_.max_stderr_bytes, r.stderr_truncated);
                    if (pfds[i].revents & (POLLHUP | POLLERR)) err_open = false;
                }
            }
        }
    }

    // Final drain after process exit.
    drainFd(stdout_pipe[0], r.stdout_text, cfg_.max_stdout_bytes, r.stdout_truncated);
    drainFd(stderr_pipe[0], r.stderr_text, cfg_.max_stderr_bytes, r.stderr_truncated);
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    int status = 0;
    if (pid > 0) {
        ::waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            r.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            r.signal = WTERMSIG(status);
            r.exit_code = 128 + r.signal;
        }
    }

    auto end = std::chrono::steady_clock::now();
    r.duration_ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                           end - start).count());

    r.ok = !r.timed_out && !r.killed && r.error.empty()
         && r.exit_code == 0 && r.signal == 0;
    if (!r.ok && r.error.empty()) {
        if (r.timed_out) r.error = "timed out";
        else if (r.killed) r.error = "killed";
        else if (r.signal) r.error = "child died from signal " + std::to_string(r.signal);
    }
    return r;
}

} // namespace arise
