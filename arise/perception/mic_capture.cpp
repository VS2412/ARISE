#include "perception/mic_capture.hpp"
#include "util/log.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace arise::audio {

namespace {

// Spawn `arecord -q -D <dev> -f S16_LE -c 1 -r <rate> -t raw` with stdout
// redirected to a pipe. Returns child pid + read fd, or {-1, -1} on failure.
struct SpawnResult { pid_t pid = -1; int read_fd = -1; };

SpawnResult spawnArecord(const std::string& device, int rate) {
    int fds[2];
    if (pipe(fds) != 0) {
        log::warn(std::string("MicCapture: pipe(): ") + std::strerror(errno));
        return {};
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Child: stdin from /dev/null
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    // Child: stdout → write end of pipe
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    // Child: stderr → /dev/null (arecord chatters)
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    char rate_buf[16];
    std::snprintf(rate_buf, sizeof rate_buf, "%d", rate);
    std::string device_copy = device;

    char  prog[]   = "arecord";
    char  flag_q[] = "-q";
    char  flag_D[] = "-D";
    char  flag_f[] = "-f";
    char  fmt[]    = "S16_LE";
    char  flag_c[] = "-c";
    char  ch[]     = "1";
    char  flag_r[] = "-r";
    char  flag_t[] = "-t";
    char  raw[]    = "raw";

    char* argv[] = { prog, flag_q,
                     flag_D, device_copy.data(),
                     flag_f, fmt,
                     flag_c, ch,
                     flag_r, rate_buf,
                     flag_t, raw,
                     nullptr };

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "arecord", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);          // parent doesn't write

    if (rc != 0) {
        log::warn(std::string("MicCapture: posix_spawnp arecord: ")
                  + std::strerror(rc));
        close(fds[0]);
        return {};
    }
    return { pid, fds[0] };
}

void killAndReap(pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    int status = 0;
    // Give it 200ms to exit gracefully, then SIGKILL.
    for (int i = 0; i < 20; ++i) {
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0) return;
        ::usleep(10 * 1000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
}

} // namespace

struct MicCapture::Impl {
    Config cfg;
    WindowCb cb;

    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};

    pid_t arecord_pid = -1;
    int   read_fd     = -1;
    std::thread worker;

    std::atomic<std::size_t> bytes_read{0};
    std::atomic<std::size_t> windows_emitted{0};

    // Rolling float32 ring buffer.
    std::vector<float> ring;
    std::size_t        write_pos     = 0;       // next write position
    std::size_t        samples_since_emit = 0;
    bool               filled_once   = false;
};

MicCapture::MicCapture() : impl_(std::make_unique<Impl>()) {}

MicCapture::~MicCapture() { stop(); }

void MicCapture::setOnWindow(WindowCb cb) {
    impl_->cb = std::move(cb);
}

bool MicCapture::running() const {
    return impl_->running.load();
}

std::size_t MicCapture::bytesRead() const {
    return impl_->bytes_read.load();
}

std::size_t MicCapture::windowsEmitted() const {
    return impl_->windows_emitted.load();
}

bool MicCapture::start(const Config& cfg) {
    if (impl_->running.load()) return false;
    if (cfg.window_samples == 0 || cfg.hop_samples == 0) return false;

    impl_->cfg = cfg;
    impl_->ring.assign(cfg.window_samples, 0.0f);
    impl_->write_pos          = 0;
    impl_->samples_since_emit = 0;
    impl_->filled_once        = false;
    impl_->bytes_read.store(0);
    impl_->windows_emitted.store(0);
    impl_->stopping.store(false);

    auto sp = spawnArecord(cfg.alsa_device, cfg.sample_rate);
    if (sp.pid < 0) return false;
    impl_->arecord_pid = sp.pid;
    impl_->read_fd     = sp.read_fd;

    impl_->running.store(true);
    impl_->worker = std::thread([this]() {
        std::vector<std::int16_t> chunk(2048);
        while (!impl_->stopping.load()) {
            ssize_t got = ::read(impl_->read_fd,
                                 chunk.data(),
                                 chunk.size() * sizeof(std::int16_t));
            if (got <= 0) {
                if (got == 0) {
                    log::warn("MicCapture: arecord EOF");
                    break;
                }
                if (errno == EINTR) continue;
                log::warn(std::string("MicCapture: read: ") + std::strerror(errno));
                break;
            }
            impl_->bytes_read.fetch_add(std::size_t(got));

            std::size_t n_samples = std::size_t(got) / sizeof(std::int16_t);
            for (std::size_t i = 0; i < n_samples; ++i) {
                impl_->ring[impl_->write_pos] = float(chunk[i]) / 32768.0f;
                impl_->write_pos = (impl_->write_pos + 1) % impl_->ring.size();
                if (impl_->write_pos == 0) impl_->filled_once = true;
                ++impl_->samples_since_emit;
            }

            while (impl_->filled_once &&
                   impl_->samples_since_emit >= impl_->cfg.hop_samples) {
                if (impl_->cb) {
                    // Materialise contiguous window starting `write_pos` (oldest
                    // sample) — hands a snapshot to the callback so the consumer
                    // can hold it past the next read without us locking the ring.
                    std::vector<float> window(impl_->ring.size());
                    for (std::size_t i = 0; i < impl_->ring.size(); ++i) {
                        window[i] = impl_->ring[(impl_->write_pos + i) %
                                                 impl_->ring.size()];
                    }
                    impl_->cb(window.data(), window.size());
                }
                impl_->windows_emitted.fetch_add(1);
                impl_->samples_since_emit -= impl_->cfg.hop_samples;
            }
        }
        impl_->running.store(false);
    });

    return true;
}

void MicCapture::stop() {
    if (!impl_) return;
    bool was_running = impl_->running.exchange(false);
    impl_->stopping.store(true);

    if (impl_->arecord_pid > 0) {
        killAndReap(impl_->arecord_pid);
        impl_->arecord_pid = -1;
    }
    if (impl_->read_fd >= 0) {
        ::close(impl_->read_fd);
        impl_->read_fd = -1;
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    (void)was_running;
}

} // namespace arise::audio
