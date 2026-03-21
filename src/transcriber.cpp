#include "transcriber.hpp"
#include "logger.hpp"
#include "whisper.h"
#include <vector>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <sstream>
#include <string_view>
#include <array>
#include <cstdio>

namespace {
    struct WavPcmS16Mono16k {
        std::vector<float> samples;
        std::string error;
    };

    static uint32_t read_u32_le(const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    static uint16_t read_u16_le(const uint8_t* p) {
        return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    }

    // Minimal WAV reader for PCM s16le; validates 16kHz mono.
    static WavPcmS16Mono16k loadWavPcmS16le16kMono(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {{}, "could not open file"};

        std::array<uint8_t, 12> riff{};
        f.read(reinterpret_cast<char*>(riff.data()), (std::streamsize)riff.size());
        if (f.gcount() != (std::streamsize)riff.size()) return {{}, "file too small"};
        if (std::string_view((char*)riff.data(), 4) != "RIFF" || std::string_view((char*)riff.data() + 8, 4) != "WAVE") {
            return {{}, "not a RIFF/WAVE file"};
        }

        uint16_t audioFormat = 0;
        uint16_t numChannels = 0;
        uint32_t sampleRate = 0;
        uint16_t bitsPerSample = 0;
        std::vector<int16_t> pcm;

        bool haveFmt = false, haveData = false;
        while (f && !(haveFmt && haveData)) {
            std::array<uint8_t, 8> hdr{};
            f.read(reinterpret_cast<char*>(hdr.data()), (std::streamsize)hdr.size());
            if (f.gcount() == 0) break;
            if (f.gcount() != (std::streamsize)hdr.size()) return {{}, "truncated chunk header"};

            const std::string_view id((char*)hdr.data(), 4);
            uint32_t size = read_u32_le(hdr.data() + 4);

            if (id == "fmt ") {
                std::vector<uint8_t> buf(size);
                f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
                if (f.gcount() != (std::streamsize)buf.size()) return {{}, "truncated fmt chunk"};
                if (buf.size() < 16) return {{}, "fmt chunk too small"};

                audioFormat = read_u16_le(buf.data() + 0);
                numChannels = read_u16_le(buf.data() + 2);
                sampleRate = read_u32_le(buf.data() + 4);
                bitsPerSample = read_u16_le(buf.data() + 14);
                haveFmt = true;
            } else if (id == "data") {
                if (size % 2 != 0) return {{}, "data chunk size not aligned"};
                std::vector<uint8_t> buf(size);
                f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
                if (f.gcount() != (std::streamsize)buf.size()) return {{}, "truncated data chunk"};

                pcm.resize(size / 2);
                std::memcpy(pcm.data(), buf.data(), size);
                haveData = true;
            } else {
                // skip unknown chunk (plus pad byte if odd)
                f.seekg((std::streamoff)size + (size & 1u), std::ios::cur);
            }
        }

        if (!haveFmt) return {{}, "missing fmt chunk"};
        if (!haveData) return {{}, "missing data chunk"};
        if (audioFormat != 1) return {{}, "unsupported WAV encoding (need PCM)"}; // 1 = PCM
        if (numChannels != 1) return {{}, "unsupported channel count (need mono)"};
        if (sampleRate != 16000) return {{}, "unsupported sample rate (need 16000 Hz)"};
        if (bitsPerSample != 16) return {{}, "unsupported bit depth (need 16-bit)"};

        std::vector<float> samples;
        samples.resize(pcm.size());
        for (size_t i = 0; i < pcm.size(); ++i) {
            samples[i] = (float)pcm[i] / 32768.0f;
        }
        return {std::move(samples), {}};
    }

    static bool envTrue(const char* v, bool defaultVal) {
        if (!v) return defaultVal;
        std::string s(v);
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return (s == "1" || s == "true" || s == "yes" || s == "on");
    }

    static std::string runCommandCaptureAll(const std::string& cmd) {
        // capture stdout+stderr; caller must ensure cmd is safe/controlled
        std::string full = cmd + " 2>&1";
        FILE* pipe = popen(full.c_str(), "r");
        if (!pipe) return "";
        std::string out;
        char buf[4096];
        while (true) {
            size_t n = fread(buf, 1, sizeof(buf), pipe);
            if (n > 0) out.append(buf, buf + n);
            if (n < sizeof(buf)) break;
        }
        pclose(pipe);
        return out;
    }

    static std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
        if (from.empty()) return s;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
        return s;
    }
}

Transcriber::Transcriber(const std::string& modelPath) {
    whisper_context_params cparams = whisper_context_default_params();
    const bool wantGpu = envTrue(std::getenv("AI_AGENT_USE_GPU"), true);
    cparams.use_gpu = wantGpu;

    ctx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    if (!ctx && wantGpu) {
        Logger::warn("Failed to init whisper with GPU; retrying CPU. Model: " + modelPath);
        cparams.use_gpu = false;
        ctx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    }
    if (!ctx) {
        Logger::error("Failed to load whisper model: " + modelPath);
        return;
    }
    Logger::info("Whisper model loaded: " + modelPath);
}

Transcriber::~Transcriber() {
    if (ctx) whisper_free(ctx);
}

std::string Transcriber::transcribe(const std::string& wavPath) {
    // Optional external command mode (prints the actual command you want to run).
    // Example:
    //   export AI_AGENT_TRANSCRIBE_CMD='/home/Aurelius/Documents/AdoVs/whisper.cpp/build/bin/whisper-cli -m /path/to/model.bin -f "{wav}" --output-txt --no-timestamps'
    if (const char* cmdEnv = std::getenv("AI_AGENT_TRANSCRIBE_CMD")) {
        std::string cmd = replaceAll(std::string(cmdEnv), "{wav}", wavPath);
        Logger::info("Transcribe command: " + cmd);
        std::string out = runCommandCaptureAll(cmd);
        if (out.empty()) {
            Logger::warn("Transcribe command produced no output.");
        }
        return out;
    }

    if (!ctx) {
        Logger::error("Transcriber: no model loaded.");
        return "";
    }

    auto wav = loadWavPcmS16le16kMono(wavPath);
    if (!wav.error.empty()) {
        Logger::error("Transcriber: invalid WAV (" + wav.error + "): " + wavPath);
        return "";
    }
    auto& samples = wav.samples;

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language   = "en";
    wparams.n_threads  = 4;       // leave cores free for other work
    wparams.print_realtime   = false;
    wparams.print_progress   = false;
    wparams.print_timestamps = false;
    wparams.single_segment   = false;

    if (whisper_full(ctx, wparams, samples.data(), (int)samples.size()) != 0) {
        Logger::error("Transcriber: whisper_full() failed.");
        return "";
    }

    std::string result;
    int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; ++i)
        result += whisper_full_get_segment_text(ctx, i);

    // trim leading space whisper often adds
    if (!result.empty() && result[0] == ' ')
        result = result.substr(1);

    return result;
}