#include "wakeword.hpp"
#include "logger.hpp"

#include <onnxruntime_cxx_api.h>
#include <numeric>
#include <algorithm>
#include <functional>
#include <vector>
#include <array>

namespace {
constexpr size_t kNumMels      = 32;  // mel bins per frame
constexpr size_t kEmbWindow    = 76;  // mel frames into embedding model
constexpr size_t kEmbStep      = 8;   // mel frames to advance per embedding
constexpr size_t kEmbFeatures  = 96;  // embedding vector size
constexpr size_t kWwFeatures   = 16;  // embeddings into wake model
}

struct WakeWord::Impl {
    WakeWord::Config cfg;

    Ort::Env        env{ORT_LOGGING_LEVEL_WARNING, "aria-wakeword"};
    Ort::SessionOptions opts;
    Ort::MemoryInfo memInfo{Ort::MemoryInfo::CreateCpu(
                        OrtArenaAllocator, OrtMemTypeDefault)};
    Ort::AllocatorWithDefaultOptions alloc;

    std::unique_ptr<Ort::Session> melSession, embSession, wwSession;

    // Input/output names copied out of ORT-allocated buffers so we don't have
    // to carry stateful-deleter unique_ptrs in struct members.
    std::string melInName,  melOutName;
    std::string embInName,  embOutName;
    std::string wwInName,   wwOutName;

    // Pipeline buffers (row-major flattened).
    std::vector<float> samplesBuf;   // raw int16-cast-to-float
    std::vector<float> melsBuf;      // frames × 32 flat
    std::vector<float> embsBuf;      // embeddings × 96 flat

    // Trigger state
    int activation = 0;
    int sinceTrigger = 0;  // frames since last fire (for refractory counting)

    bool ready = false;

    Impl() {
        // Keep ORT single-threaded — wake inference is light and mixing thread
        // pools with the main audio thread hurts latency.
        opts.SetIntraOpNumThreads(1);
        opts.SetInterOpNumThreads(1);
        env.DisableTelemetryEvents();
    }
};

WakeWord::WakeWord()  = default;
WakeWord::~WakeWord() = default;

bool WakeWord::isReady() const { return impl_ && impl_->ready; }

bool WakeWord::init(const Config& cfg) {
    impl_ = std::make_unique<Impl>();
    impl_->cfg = cfg;

    try {
        impl_->melSession = std::make_unique<Ort::Session>(
            impl_->env, cfg.melspecModelPath.c_str(), impl_->opts);
        impl_->embSession = std::make_unique<Ort::Session>(
            impl_->env, cfg.embeddingModelPath.c_str(), impl_->opts);
        impl_->wwSession  = std::make_unique<Ort::Session>(
            impl_->env, cfg.wakewordModelPath.c_str(), impl_->opts);

        auto copyName = [&](Ort::AllocatedStringPtr p) -> std::string {
            return std::string(p.get());
        };
        impl_->melInName  = copyName(impl_->melSession->GetInputNameAllocated(0, impl_->alloc));
        impl_->melOutName = copyName(impl_->melSession->GetOutputNameAllocated(0, impl_->alloc));
        impl_->embInName  = copyName(impl_->embSession->GetInputNameAllocated(0, impl_->alloc));
        impl_->embOutName = copyName(impl_->embSession->GetOutputNameAllocated(0, impl_->alloc));
        impl_->wwInName   = copyName(impl_->wwSession->GetInputNameAllocated(0, impl_->alloc));
        impl_->wwOutName  = copyName(impl_->wwSession->GetOutputNameAllocated(0, impl_->alloc));
    } catch (const Ort::Exception& e) {
        Logger::error(std::string("WakeWord: ORT init failed: ") + e.what());
        impl_.reset();
        return false;
    }

    // Pre-allocate pipeline buffers; sizes are approximate steady-state.
    impl_->samplesBuf.reserve(kChunkSamples * 2);
    impl_->melsBuf.reserve(kEmbWindow * kNumMels * 4);
    impl_->embsBuf.reserve(kWwFeatures * kEmbFeatures * 2);

    impl_->ready = true;

    // Pre-warm the wake-model buffer with silence-derived embeddings so the
    // detector can fire on the very first real audio. Without this, cold
    // start takes ~2s to accumulate 16 embeddings.
    {
        std::vector<int16_t> silence(kChunkSamples, 0);
        for (int i = 0; i < 40; ++i)
            processChunk(silence.data(), silence.size());
        impl_->activation = 0;
        impl_->sinceTrigger = 0;
        lastProb_ = 0.0f;
    }
    Logger::info("WakeWord: loaded models (model='" +
                 cfg.wakewordModelPath + "', threshold=" +
                 std::to_string(cfg.threshold) + ")");
    return true;
}

void WakeWord::reset() {
    if (!impl_) return;
    impl_->samplesBuf.clear();
    impl_->melsBuf.clear();
    impl_->embsBuf.clear();
    impl_->activation   = 0;
    impl_->sinceTrigger = 0;
    lastProb_           = 0.0f;
}

bool WakeWord::processChunk(const int16_t* samples, size_t n) {
    if (!impl_ || !impl_->ready) return false;
    if (n == 0) return false;

    auto& I = *impl_;
    const char* melIn  = I.melInName.c_str();
    const char* melOut = I.melOutName.c_str();
    const char* embIn  = I.embInName.c_str();
    const char* embOut = I.embOutName.c_str();
    const char* wwIn   = I.wwInName.c_str();
    const char* wwOut  = I.wwOutName.c_str();

    bool triggered = false;

    try {
        // ── Stage 1: append raw samples (cast to float, no normalization) ──
        I.samplesBuf.reserve(I.samplesBuf.size() + n);
        for (size_t i = 0; i < n; ++i)
            I.samplesBuf.push_back(static_cast<float>(samples[i]));

        // ── Stage 2: melspectrogram ──
        // Run in exactly 1280-sample chunks. Output frames per call vary with
        // the ONNX model's internal padding but are stable enough for
        // streaming wake detection.
        while (I.samplesBuf.size() >= kChunkSamples) {
            std::array<int64_t, 2> shape{1, static_cast<int64_t>(kChunkSamples)};

            Ort::Value in = Ort::Value::CreateTensor<float>(
                I.memInfo, I.samplesBuf.data(), kChunkSamples,
                shape.data(), shape.size());

            auto outs = I.melSession->Run(
                Ort::RunOptions{nullptr},
                &melIn,  &in,  1,
                &melOut, 1);

            auto& mel = outs.front();
            auto info = mel.GetTensorTypeAndShapeInfo();
            auto dims = info.GetShape();
            const float* data = mel.GetTensorData<float>();
            size_t count = std::accumulate(dims.begin(), dims.end(),
                                           size_t{1}, std::multiplies<>());

            // Scale to match the numerical range the embedding model expects.
            I.melsBuf.reserve(I.melsBuf.size() + count);
            for (size_t i = 0; i < count; ++i)
                I.melsBuf.push_back(data[i] / 10.0f + 2.0f);

            I.samplesBuf.erase(I.samplesBuf.begin(),
                               I.samplesBuf.begin() + kChunkSamples);
        }

        // ── Stage 3: embeddings — slide 76-frame window, step 8 ──
        while (I.melsBuf.size() / kNumMels >= kEmbWindow) {
            std::array<int64_t, 4> shape{
                1, static_cast<int64_t>(kEmbWindow),
                static_cast<int64_t>(kNumMels), 1};

            Ort::Value in = Ort::Value::CreateTensor<float>(
                I.memInfo, I.melsBuf.data(), kEmbWindow * kNumMels,
                shape.data(), shape.size());

            auto outs = I.embSession->Run(
                Ort::RunOptions{nullptr},
                &embIn,  &in,  1,
                &embOut, 1);

            auto& emb = outs.front();
            auto info = emb.GetTensorTypeAndShapeInfo();
            auto dims = info.GetShape();
            const float* data = emb.GetTensorData<float>();
            size_t count = std::accumulate(dims.begin(), dims.end(),
                                           size_t{1}, std::multiplies<>());

            // Model returns (1,1,1,96). Append the 96 floats.
            I.embsBuf.insert(I.embsBuf.end(), data, data + count);

            I.melsBuf.erase(I.melsBuf.begin(),
                            I.melsBuf.begin() + (kEmbStep * kNumMels));
        }

        // ── Stage 4: wake model — 16-embedding sliding window ──
        while (I.embsBuf.size() / kEmbFeatures >= kWwFeatures) {
            std::array<int64_t, 3> shape{
                1, static_cast<int64_t>(kWwFeatures),
                static_cast<int64_t>(kEmbFeatures)};

            Ort::Value in = Ort::Value::CreateTensor<float>(
                I.memInfo, I.embsBuf.data(), kWwFeatures * kEmbFeatures,
                shape.data(), shape.size());

            auto outs = I.wwSession->Run(
                Ort::RunOptions{nullptr},
                &wwIn,  &in,  1,
                &wwOut, 1);

            auto& ww = outs.front();
            auto info = ww.GetTensorTypeAndShapeInfo();
            auto dims = info.GetShape();
            const float* data = ww.GetTensorData<float>();
            size_t count = std::accumulate(dims.begin(), dims.end(),
                                           size_t{1}, std::multiplies<>());
            if (count == 0) break;

            // Output is usually (1,1) — take max probability across any
            // multi-class wake models.
            float prob = data[0];
            for (size_t i = 1; i < count; ++i)
                prob = std::max(prob, data[i]);
            lastProb_ = prob;

            if (I.cfg.debug)
                Logger::info("WakeWord: prob=" + std::to_string(prob));

            // Activation / refractory state machine (mirrors rhasspy impl).
            if (I.sinceTrigger > 0) {
                // Cooling down after a prior fire.
                if (--I.sinceTrigger == 0) I.activation = 0;
            } else if (prob > I.cfg.threshold) {
                if (++I.activation >= I.cfg.triggerLevel) {
                    triggered = true;
                    I.sinceTrigger = I.cfg.refractoryFrames;
                    I.activation   = 0;
                }
            } else if (I.activation > 0) {
                --I.activation;
            }

            // Advance window by one embedding.
            I.embsBuf.erase(I.embsBuf.begin(),
                            I.embsBuf.begin() + kEmbFeatures);
        }
    } catch (const Ort::Exception& e) {
        Logger::error(std::string("WakeWord: ORT run failed: ") + e.what());
        return false;
    }

    return triggered;
}
