#pragma once
#include <string>

// All configurable paths and tunables. Populated once at startup from (in order):
// defaults → ~/.config/aria/config.toml → environment variables.
struct AriaConfig {
    std::string whisper_model;
    std::string piper_model;
    std::string ollama_model;
    std::string ollama_url;
    std::string db_path;
    std::string log_path;
    std::string sqlite_vec_path;   // Phase 8: loadable vec0.so path
    std::string embedding_model;   // Phase 8: ollama embed model (nomic-embed-text)

    // Phase 9: Audio wake word (openWakeWord via ONNX Runtime).
    std::string wake_model_dir;    // dir containing melspec + embedding + <wake>.onnx
    std::string wake_model;        // filename of the wake-word model inside wake_model_dir
    std::string wake_cue;          // "tone" | "voice" | "none" — feedback on wake
    float wake_threshold  = 0.5f;  // prob > threshold counts as a hot frame
    int   wake_trigger    = 4;     // consecutive hot frames before firing
    int   wake_refractory = 20;    // cooldown frames after a fire
    int   wake_window_sec = 10;    // how long we stay "awake" after a trigger
    bool  wake_enabled    = true;  // master switch — falls back to always-on if off
    bool  wake_debug      = false; // log per-frame probabilities

    int vad_mode         = 3;
    int max_react_steps  = 5;
    int llm_timeout_sec  = 60;
    int volume_step      = 5;
    int brightness_step  = 10;
    int embedding_dim    = 768;   // must match embedding_model output
    bool vec_backfill    = true;  // run one-time embed backfill on startup
    bool whisper_gpu     = false; // Phase 9: default off — wake gates transcription now
    bool always_listen   = false; // Phase 9: if true, bypass wake word entirely

    // Phase 10: Multimodal Vision (Ollama VLM backend).
    std::string vlm_model          = "moondream"; // ollama pull moondream (or minicpm-v)
    int         vlm_timeout_sec    = 20;          // VLM calls can swap models — wide budget
    int         vision_cache_ttl_sec = 60;        // same screen+question cache reuse window
};

class Config {
public:
    static void load();                // call once at startup
    static const AriaConfig& get();    // load-on-first-access if not yet loaded

private:
    static AriaConfig cfg_;
    static bool       loaded_;
};
