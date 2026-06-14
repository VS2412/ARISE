#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace arise::audio {

// Coarse scene buckets ARISE actually cares about. AudioSet has 521 classes;
// we collapse them into a small set so blackboard transitions are meaningful
// instead of being a torrent of "Wood, Speech, Wood" flicker.
enum class Scene {
    Unknown,
    Silence,
    Speech,
    Music,
    Typing,
    Phone,
    Doorbell,
    Alarm,
    Laughter,
    Other,
};

const char* sceneToString(Scene s);
Scene       sceneFromString(const std::string& s);

// Map a YAMNet class display_name (e.g. "Speech", "Computer keyboard",
// "Doorbell") to a coarse Scene. Substring-matched against a curated table —
// see audio_scene.cpp for the full mapping.
Scene mapClassToScene(const std::string& display_name);

// Read yamnet_class_map.csv. CSV columns: index, mid, display_name.
// Returns empty vector on parse error. Display names with embedded commas are
// quoted in the source CSV; the parser handles that.
std::vector<std::string> loadClassMap(const std::string& csv_path);

// YAMNet wrapper. Loads the ONNX model once; classify() runs a single
// inference on a fixed-length float32 mono 16 kHz window.
//
// The model accepts arbitrary-length waveform input but YAMNet's mel-frame
// stride means short windows give one frame and longer windows give several.
// We default to 15600 samples (≈ 0.975 s @ 16 kHz) which is the canonical
// YAMNet "one frame" window — keeps post-processing trivial (one 521-dim
// vector out, no need to average across frames).
class SceneClassifier {
public:
    static constexpr int    kSampleRate    = 16000;
    static constexpr std::size_t kWindowSamples = 15600;  // ≈ 0.975 s
    static constexpr int    kNumClasses    = 521;

    struct Config {
        std::string model_path;        // path to yamnet.onnx
        std::string labels_path;       // path to yamnet_class_map.csv
        float       min_score = 0.10f; // top-1 score below this → Unknown
        int         intra_threads = 1;
    };

    struct Result {
        Scene       scene     = Scene::Unknown;
        float       score     = 0.0f;     // softmax/sigmoid output of top-1
        int         class_idx = -1;
        std::string display_name;         // raw YAMNet label
    };

    SceneClassifier();
    ~SceneClassifier();
    SceneClassifier(const SceneClassifier&)            = delete;
    SceneClassifier& operator=(const SceneClassifier&) = delete;

    // Loads ONNX session + label CSV. Logs and returns false on failure.
    bool init(const Config& cfg);
    bool isReady() const;

    // Classify one window. `samples` is float32 in [-1, 1], length must equal
    // kWindowSamples. Returns Unknown on size mismatch or top-1 below
    // cfg.min_score.
    Result classify(const float* samples, std::size_t n);

    // Convenience: convert int16 PCM → float in-place-ish, then classify.
    Result classifyPcm16(const std::int16_t* pcm, std::size_t n);

    // For tests: number of class labels actually loaded.
    std::size_t classCount() const;

    const Config& config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace arise::audio
