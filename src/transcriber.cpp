#include "transcriber.hpp"
#include "logger.hpp"
#include "config.hpp"
#include "whisper.h"
#include <vector>
#include <fstream>
#include <cstring>
#include <chrono>

Transcriber::Transcriber(const std::string& modelPath) {
    whisper_context_params cparams = whisper_context_default_params();
    // Phase 9: default to CPU. Wake-word now gates transcription so whisper
    // runs only on demand; keeping GPU resident would starve Ollama.
    cparams.use_gpu = Config::get().whisper_gpu;
    ctx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    if (!ctx) { Logger::error("Whisper: failed to load model: " + modelPath); return; }
    Logger::info(std::string("Whisper model loaded (") +
                 (cparams.use_gpu ? "GPU" : "CPU") + "): " + modelPath);
}

Transcriber::~Transcriber() {
    if (ctx) whisper_free(ctx);
}

static std::vector<float> loadWav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};

    char riff[4];
    f.read(riff, 4);
    if (std::strncmp(riff, "RIFF", 4) != 0) return {};

    f.seekg(12); // skip "RIFF", file size, "WAVE"

    // scan chunks until "data"
    while (f) {
        char id[4];
        uint32_t chunkSize = 0;
        f.read(id, 4);
        f.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (!f) break;

        if (std::strncmp(id, "data", 4) == 0) {
            std::vector<int16_t> raw(chunkSize / sizeof(int16_t));
            f.read(reinterpret_cast<char*>(raw.data()), chunkSize);
            std::vector<float> samples(raw.size());
            for (size_t i = 0; i < raw.size(); ++i)
                samples[i] = raw[i] / 32768.0f;
            return samples;
        }
        f.seekg(chunkSize, std::ios::cur); // skip unknown chunk
    }
    return {};
}

std::string Transcriber::transcribe(const std::string& wavPath) {
    if (!ctx) { Logger::error("Transcriber: no model loaded."); return ""; }

    auto samples = loadWav(wavPath);
    if (samples.empty()) {
        Logger::error("Transcriber: could not read WAV or no audio data: " + wavPath);
        return "";
    }

    // Too short to be real speech — skip whisper to save GPU cycles.
    // 1000 samples @ 16kHz = 62.5ms; any VAD-passed utterance shorter than
    // that is almost always a click or doorbell that slipped through.
    if (samples.size() < 1000) {
        Logger::info("Transcriber: skipping short audio (" +
                     std::to_string(samples.size()) + " samples)");
        return "";
    }

    Logger::info("Transcribing file: " + wavPath +
                 " (" + std::to_string(samples.size()) + " samples)");

    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.language          = "en";
    wp.n_threads         = 4;
    wp.print_realtime    = false;
    wp.print_progress    = false;
    wp.print_timestamps  = false;
    wp.single_segment    = false;
    wp.no_speech_thold   = 0.3f;
    wp.no_context        = true;  // each utterance is independent; skip KV carryover
    wp.suppress_blank    = true;
    wp.initial_prompt    = "open firefox, open terminal, open code, volume up, volume down, switch workspace, run command, type text"; // command vocabulary hint

    auto t0 = std::chrono::steady_clock::now();
    if (whisper_full(ctx, wp, samples.data(), (int)samples.size()) != 0) {
        ++consecutiveFailures_;
        Logger::error("Transcriber: whisper_full() failed (fail #" +
                      std::to_string(consecutiveFailures_) + ").");
        return "";
    }
    consecutiveFailures_ = 0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    Logger::info("Transcribe finished in " + std::to_string(ms) + " ms");

    std::string result;
    int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; ++i)
        result += whisper_full_get_segment_text(ctx, i);

    if (!result.empty() && result[0] == ' ')
        result = result.substr(1);

    return result;
}