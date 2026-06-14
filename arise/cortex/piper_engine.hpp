#pragma once

#include <chrono>
#include <string>

#include "cortex/speech.hpp"

namespace arise {

// `piper` + `aplay` shell-out engine.
//
// Pipeline per call (one piper + one aplay child piped together):
//
//   piper --model <m> --output_raw
//         --length_scale <p.length_scale>
//         --noise_scale  <p.noise_scale>
//         --noise_w      <p.noise_w>
//         --sentence_silence <p.sentence_silence_sec>
//      | aplay -q -r <sample_rate> -f S16_LE -t raw -
//
// stdin to `piper` is the sentence to speak. `aplay` plays the raw PCM
// stream. `play_audio=false` discards aplay (useful in tests / CI). All
// timing comes from the wall-clock around the spawn — piper provides no
// internal timing handle.
class PiperEngine : public TtsEngine {
public:
    struct Config {
        std::string piper_bin    = "piper";
        std::string aplay_bin    = "aplay";
        std::string default_model_path;     // ~/.local/share/piper/...onnx (or absolute)
        int         sample_rate  = 22050;   // Lessac default
        bool        play_audio   = true;    // false → drop output (tests)
        std::chrono::seconds timeout { 30 };
    };

    explicit PiperEngine(Config cfg);

    // Probe: piper + (when play_audio) aplay both on PATH and the model
    // file (or override `params.voice`) is readable.
    bool        isAvailable() const override;

    Result      speak(std::string_view text, const TtsParams& params) override;

    std::string name() const override { return "piper"; }

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

} // namespace arise
