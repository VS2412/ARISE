#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace arise {

// bwrap-isolated child process runner.
//
// Default profile: read-only bind of `/`, fresh `/tmp`, fresh `/proc`,
// `/dev` device passthrough, **no network**, no environment unless the
// caller passes one explicitly, parent-death tied. Stdout / stderr are
// captured into the result up to a byte cap. Wall-clock timeout SIGKILLs
// the child (and its process group, since `bwrap --die-with-parent` plus
// our own SIGKILL handle the cleanup).
//
// All ARISE-spawned tool execution funnels through this — Phase 5 commit 1
// tools, Phase 5 commit 2 forge dry-runs, and any future agent that needs
// to run user-bound code touches it. If `bwrap` is missing the run() call
// returns ok=false with `error="bwrap unavailable"` rather than silently
// falling back to an unprivileged exec — we never trade isolation for
// convenience.
class Sandbox {
public:
    struct Config {
        std::string             bwrap_path     = "bwrap";
        bool                    allow_network  = false;
        // Bind-mounted read-write into the sandbox. Each entry is treated as
        // both source and target path. Must already exist on the host.
        std::vector<std::string> writable_paths;
        // Bind-mounted read-only. Useful when a script lives under /tmp
        // (which is shadowed by the fresh tmpfs) — bind the script's parent
        // dir read-only so the binary remains visible inside the sandbox.
        std::vector<std::string> readonly_paths;
        // Working directory inside the sandbox. Empty == bwrap default.
        std::string             chdir_in_sandbox;
        // Wall-clock budget. Stored as milliseconds so tests + tool
        // configs can ask for sub-second timeouts; integer seconds are
        // promoted implicitly via the chrono duration_cast rules.
        std::chrono::milliseconds timeout    { 10000 };
        std::size_t             max_stdout_bytes = 64 * 1024;
        std::size_t             max_stderr_bytes = 16 * 1024;
        // When non-empty, replaces inherited environment with these
        // exact KEY=VAL strings. Empty vector means "no env" — the caller
        // gets a sterile environment.
        std::vector<std::string> env;
    };

    struct Result {
        bool        ok           = false;     // exit_code == 0 + not timed out + no sandbox error
        bool        timed_out    = false;
        bool        killed       = false;     // cooperative kill signalled
        int         exit_code    = -1;
        int         signal       = 0;         // non-zero if child died from a signal
        std::string stdout_text;
        std::string stderr_text;
        bool        stdout_truncated = false;
        bool        stderr_truncated = false;
        int         duration_ms  = 0;
        std::string error;                     // sandbox-level error (bwrap missing, spawn failed)
    };

    explicit Sandbox(Config cfg);

    // Run argv (must be non-empty; argv[0] is the binary inside the sandbox).
    // `kill` is consulted before spawn + while polling; cooperative cancel.
    Result run(const std::vector<std::string>& argv,
               const std::string& stdin_text = "",
               std::atomic<bool>* kill = nullptr) const;

    // True if the configured bwrap binary is on PATH (or absolute and exec).
    static bool isAvailable(const std::string& bwrap_path = "bwrap");

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

} // namespace arise
