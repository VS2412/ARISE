#include "perception/audio_scene.hpp"
#include "util/log.hpp"

#include <fstream>

#include <onnxruntime_cxx_api.h>

namespace arise::audio {

const char* sceneToString(Scene s) {
    switch (s) {
        case Scene::Silence:  return "silence";
        case Scene::Speech:   return "speech";
        case Scene::Music:    return "music";
        case Scene::Typing:   return "typing";
        case Scene::Phone:    return "phone";
        case Scene::Doorbell: return "doorbell";
        case Scene::Alarm:    return "alarm";
        case Scene::Laughter: return "laughter";
        case Scene::Other:    return "other";
        case Scene::Unknown:  return "unknown";
    }
    return "unknown";
}

Scene sceneFromString(const std::string& s) {
    if (s == "silence")  return Scene::Silence;
    if (s == "speech")   return Scene::Speech;
    if (s == "music")    return Scene::Music;
    if (s == "typing")   return Scene::Typing;
    if (s == "phone")    return Scene::Phone;
    if (s == "doorbell") return Scene::Doorbell;
    if (s == "alarm")    return Scene::Alarm;
    if (s == "laughter") return Scene::Laughter;
    if (s == "other")    return Scene::Other;
    return Scene::Unknown;
}

namespace {

// Lowercase ASCII a-Z for dumb substring matching.
std::string toLower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return s;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

Scene mapClassToScene(const std::string& display_name) {
    std::string n = toLower(display_name);

    // Order matters — most specific first so "Speech synthesizer" doesn't get
    // grabbed by a generic music match.
    if (contains(n, "silence"))               return Scene::Silence;

    if (contains(n, "doorbell") || contains(n, "ding-dong")
        || contains(n, "buzzer") || contains(n, "knock"))
        return Scene::Doorbell;

    if (contains(n, "telephone") || contains(n, "ringtone")
        || contains(n, "dial tone") || contains(n, "busy signal"))
        return Scene::Phone;

    if (contains(n, "alarm") || contains(n, "siren")
        || contains(n, "smoke detector") || contains(n, "fire alarm")
        || contains(n, "civil defense siren"))
        return Scene::Alarm;

    if (contains(n, "laughter") || contains(n, "giggle")
        || contains(n, "chuckle") || contains(n, "snicker"))
        return Scene::Laughter;

    if (contains(n, "computer keyboard") || contains(n, "typing")
        || contains(n, "typewriter") || contains(n, "keypress")
        || contains(n, "mouse click"))
        return Scene::Typing;

    if (contains(n, "speech") || contains(n, "conversation")
        || contains(n, "narration") || contains(n, "monologue")
        || contains(n, "whisper") || contains(n, "babbling")
        || contains(n, "child speech") || contains(n, "kid speaking"))
        return Scene::Speech;

    if (contains(n, "music")     || contains(n, "song")
        || contains(n, "singing") || contains(n, "guitar")
        || contains(n, "piano")   || contains(n, "drum")
        || contains(n, "bass")    || contains(n, "violin")
        || contains(n, "synthesizer") || contains(n, "saxophone")
        || contains(n, "trumpet") || contains(n, "orchestra")
        || contains(n, "rock")    || contains(n, "pop")
        || contains(n, "jazz")    || contains(n, "techno")
        || contains(n, "rap")     || contains(n, "hip hop"))
        return Scene::Music;

    return Scene::Other;
}

std::vector<std::string> loadClassMap(const std::string& csv_path) {
    std::ifstream f(csv_path);
    if (!f) return {};

    std::vector<std::string> out;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        // Strip Windows CR — YAMNet's published CSV is CRLF.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) { first = false; continue; } // header

        // Naive CSV: walk char by char, respecting quoted fields. We only need
        // the third column (display_name). YAMNet's CSV uses "..." for entries
        // that contain commas (e.g. "Child speech, kid speaking").
        int field = 0;
        std::string cur;
        bool in_quotes = false;
        std::string display;
        for (std::size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (in_quotes) {
                if (c == '"') {
                    // RFC 4180-style: "" inside quoted field means literal "
                    if (i + 1 < line.size() && line[i+1] == '"') {
                        cur.push_back('"');
                        ++i;
                    } else {
                        in_quotes = false;
                    }
                } else {
                    cur.push_back(c);
                }
            } else {
                if (c == ',') {
                    if (field == 2) { display = cur; cur.clear(); break; }
                    cur.clear();
                    ++field;
                } else if (c == '"') {
                    in_quotes = true;
                } else {
                    cur.push_back(c);
                }
            }
        }
        if (display.empty() && field == 2) display = cur;
        out.push_back(std::move(display));
    }
    return out;
}

// ─── ONNX session impl ─────────────────────────────────────────────────────

struct SceneClassifier::Impl {
    Config cfg;

    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "arise-audio-scene"};
    Ort::SessionOptions sessOpts;
    Ort::MemoryInfo mem{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
                                                   OrtMemTypeDefault)};
    Ort::AllocatorWithDefaultOptions alloc;

    std::unique_ptr<Ort::Session> session;
    std::string in_name;
    std::string out_scores_name;          // we only need the scores output

    std::vector<std::string> labels;
    std::vector<Scene>       scene_for_class;   // precomputed mapping
    bool                     ready = false;

    Impl() {
        sessOpts.SetIntraOpNumThreads(1);
        sessOpts.SetInterOpNumThreads(1);
        env.DisableTelemetryEvents();
    }
};

SceneClassifier::SceneClassifier() = default;
SceneClassifier::~SceneClassifier() = default;

bool SceneClassifier::isReady() const { return impl_ && impl_->ready; }
std::size_t SceneClassifier::classCount() const {
    return impl_ ? impl_->labels.size() : 0;
}
const SceneClassifier::Config& SceneClassifier::config() const {
    static const Config kEmpty{};
    return impl_ ? impl_->cfg : kEmpty;
}

bool SceneClassifier::init(const Config& cfg) {
    impl_ = std::make_unique<Impl>();
    impl_->cfg = cfg;
    if (cfg.intra_threads > 0) {
        impl_->sessOpts.SetIntraOpNumThreads(cfg.intra_threads);
    }

    impl_->labels = loadClassMap(cfg.labels_path);
    if (impl_->labels.empty()) {
        log::warn("AudioScene: failed to load labels from " + cfg.labels_path);
        return false;
    }
    impl_->scene_for_class.resize(impl_->labels.size());
    for (std::size_t i = 0; i < impl_->labels.size(); ++i) {
        impl_->scene_for_class[i] = mapClassToScene(impl_->labels[i]);
    }

    try {
        impl_->session = std::make_unique<Ort::Session>(
            impl_->env, cfg.model_path.c_str(), impl_->sessOpts);
        impl_->in_name = std::string(
            impl_->session->GetInputNameAllocated(0, impl_->alloc).get());
        impl_->out_scores_name = std::string(
            impl_->session->GetOutputNameAllocated(0, impl_->alloc).get());
    } catch (const std::exception& e) {
        log::warn(std::string("AudioScene: ORT session init failed: ") + e.what());
        return false;
    }

    impl_->ready = true;
    return true;
}

SceneClassifier::Result SceneClassifier::classify(const float* samples,
                                                  std::size_t n) {
    Result r;
    if (!isReady() || !samples || n != kWindowSamples) return r;

    // We pass the buffer through as-is; YAMNet wants float32 in [-1,1].
    const std::int64_t shape[1] = { std::int64_t(n) };
    Ort::Value in = Ort::Value::CreateTensor<float>(
        impl_->mem, const_cast<float*>(samples), n, shape, 1);

    const char* in_names[]  = { impl_->in_name.c_str() };
    const char* out_names[] = { impl_->out_scores_name.c_str() };

    std::vector<Ort::Value> outs;
    try {
        outs = impl_->session->Run(Ort::RunOptions{nullptr},
                                   in_names, &in, 1,
                                   out_names, 1);
    } catch (const std::exception& e) {
        log::warn(std::string("AudioScene: ORT run: ") + e.what());
        return r;
    }
    if (outs.empty()) return r;

    auto info = outs[0].GetTensorTypeAndShapeInfo();
    auto shp  = info.GetShape();
    // Expected (N_frames, 521); usually N_frames == 1 for our window size.
    if (shp.size() != 2 || shp.back() != kNumClasses) {
        log::warn("AudioScene: unexpected output shape");
        return r;
    }
    std::int64_t n_frames = shp[0];
    const float* data = outs[0].GetTensorData<float>();

    // Average frame scores if more than one (defensive — usually n_frames==1).
    int   best_idx   = -1;
    float best_score = -1.0f;
    for (int c = 0; c < kNumClasses; ++c) {
        float sum = 0.0f;
        for (std::int64_t fr = 0; fr < n_frames; ++fr) {
            sum += data[fr * kNumClasses + c];
        }
        float avg = sum / float(n_frames);
        if (avg > best_score) { best_score = avg; best_idx = c; }
    }

    if (best_idx < 0 || best_idx >= int(impl_->labels.size())) return r;
    if (best_score < impl_->cfg.min_score)                     return r;

    r.class_idx    = best_idx;
    r.display_name = impl_->labels[best_idx];
    r.score        = best_score;
    r.scene        = impl_->scene_for_class[best_idx];
    return r;
}

SceneClassifier::Result SceneClassifier::classifyPcm16(const std::int16_t* pcm,
                                                       std::size_t n) {
    if (!pcm || n != kWindowSamples) return {};
    std::vector<float> buf(n);
    for (std::size_t i = 0; i < n; ++i) {
        buf[i] = float(pcm[i]) / 32768.0f;
    }
    return classify(buf.data(), buf.size());
}

} // namespace arise::audio
