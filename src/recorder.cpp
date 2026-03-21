#include "recorder.hpp"
#include "logger.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>
#include <cstdlib>
#include <string>

namespace {
    static std::string shQuote(const std::string& s) {
        // POSIX shell single-quote escaping: ' -> '\'' 
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }
}

Recorder::Recorder(const std::string& path) : outputPath(path) {}

void Recorder::start() {
    if (recordPid != -1) {
        Logger::warn("Already recording.");
        return;
    }
    recordPid = fork();
    if (recordPid == 0) {
        // child process — replace itself with pw-record
        execl("/usr/bin/pw-record", "pw-record",
              "--target=57",
              "--channels=1",
              "--rate=16000",
              "--format=s16",
              outputPath.c_str(),
              nullptr);
        _exit(1); // only reached if execl fails
    }
    if (recordPid < 0) {
        Logger::error("fork() failed. Cannot start recording.");
        recordPid = -1;
        return;
    }
    Logger::info("Recording started.");
}

void Recorder::stop() {
    if (recordPid == -1) {
        Logger::warn("Not currently recording.");
        return;
    }
    kill(recordPid, SIGINT);
    waitpid(recordPid, nullptr, 0);
    recordPid = -1;

    // convert to whisper-compatible format: 16kHz mono PCM s16le
    const std::string in  = shQuote(outputPath);
    const std::string tmp = shQuote(outputPath + ".converted.wav");
    std::string cmd = "ffmpeg -y -hide_banner -loglevel error -i " + in +
                      " -ar 16000 -ac 1 -c:a pcm_s16le " + tmp +
                      " && mv -f " + tmp + " " + in;
    int rc = system(cmd.c_str());
    if (rc != 0) {
        Logger::warn("ffmpeg conversion failed (rc=" + std::to_string(rc) + "). Command: " + cmd);
    }

    Logger::info("Recording stopped. WAV saved: " + outputPath);
}

void Recorder::toggle() {
    isRecording() ? stop() : start();
}

bool Recorder::isRecording() const {
    return recordPid != -1;
}

const std::string& Recorder::getOutputPath() const {
    return outputPath;
}