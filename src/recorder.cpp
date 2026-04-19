#include "recorder.hpp"
#include "wakeword.hpp"
#include "logger.hpp"
#include "config.hpp"
#include <fvad.h>
#include <cstdio>
#include <fstream>
#include <unistd.h>
#include <cmath>
#include <deque>

Recorder::Recorder(const std::string& path, SpeechCallback cb)
    : outputPath_(path), onSpeech_(cb) {}

Recorder::~Recorder() { stop(); }

void Recorder::start() {
    if (active_.load()) return;
    active_.store(true);
    muted_.store(false);
    thread_ = std::thread(&Recorder::captureLoop, this);
    const char* label = (mode_ == Mode::WAKE_WORD && wakeWord_)
                      ? "WAKE_WORD (gated)"
                      : "ALWAYS_ON";
    Logger::info(std::string("Recorder: WebRTC VAD started — mode=") + label + ".");
}

void Recorder::stop() {
    active_.store(false);
    if (thread_.joinable()) thread_.join();
}

void Recorder::mute()   { muted_.store(true); }
void Recorder::unmute() { muted_.store(false); }

void Recorder::setWakeWord(Mode mode,
                           std::shared_ptr<WakeWord> ww,
                           std::chrono::seconds awakeWindow,
                           WakeCallback onWake) {
    mode_         = mode;
    wakeWord_     = std::move(ww);
    awakeWindow_  = awakeWindow;
    onWake_       = std::move(onWake);
    // Start muted-from-VAD; the first wake fires the gate open.
    awakeUntilMs_.store(0);
}

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Recorder::isAwake() const {
    if (mode_ == Mode::ALWAYS_ON || !wakeWord_) return true;
    return nowMs() < awakeUntilMs_.load();
}

void Recorder::armAwake() {
    if (mode_ == Mode::ALWAYS_ON || !wakeWord_) return;
    awakeUntilMs_.store(nowMs() + awakeWindow_.count() * 1000LL);
}

void Recorder::writeWav(const std::vector<int16_t>& samples) {
    std::ofstream f(outputPath_, std::ios::binary);
    if (!f) { Logger::error("Recorder: cannot write WAV"); return; }
    uint32_t dataSize = samples.size() * sizeof(int16_t);
    uint32_t fileSize = 36 + dataSize;
    uint16_t pcm=1, ch=1, bits=16, align=2;
    uint32_t rate=RATE, brate=RATE*2, fmtSize=16;
    f.write("RIFF",4); f.write((char*)&fileSize,4);
    f.write("WAVE",4); f.write("fmt ",4);
    f.write((char*)&fmtSize,4); f.write((char*)&pcm,2);
    f.write((char*)&ch,2);      f.write((char*)&rate,4);
    f.write((char*)&brate,4);   f.write((char*)&align,2);
    f.write((char*)&bits,2);    f.write("data",4);
    f.write((char*)&dataSize,4);
    f.write((char*)samples.data(), dataSize);
}

void Recorder::captureLoop() {
    // init WebRTC VAD
    Fvad* vad = fvad_new();
    if (!vad) { Logger::error("Recorder: fvad_new failed"); active_.store(false); return; }
    fvad_set_mode(vad, Config::get().vad_mode); // 0-3 (aggressive); configurable
    fvad_set_sample_rate(vad, RATE);

    // parec gives raw s16le stereo 48kHz — we resample per frame
    const char* cmd = "parec --format=s16le --rate=16000 --channels=1 --raw 2>/dev/null";
    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        Logger::error("Recorder: parec failed");
        fvad_free(vad);
        active_.store(false);
        return;
    }

    enum class State { IDLE, ONSET, SPEECH, TRAIL };
    State state = State::IDLE;

    std::deque<std::vector<int16_t>> preroll;
    std::vector<int16_t> speech;
    int onsetCount = 0;
    int trailCount = 0;

    std::vector<int16_t> frame(FRAME_SAMPLES);

    // Wake-word accumulator: 4 × 20ms frames = 1280 samples = one wake chunk.
    std::vector<int16_t> wakeBuf;
    const bool useWake = (mode_ == Mode::WAKE_WORD) && wakeWord_ && wakeWord_->isReady();
    if (useWake) wakeBuf.reserve(WAKE_CHUNK_SAMPLES);

    bool wasAwake = !useWake;  // start "awake" only if we're not gating

    while (active_.load()) {
        size_t got = fread(frame.data(), sizeof(int16_t), FRAME_SAMPLES, pipe);
        if (got < static_cast<size_t>(FRAME_SAMPLES)) break;

        if (muted_.load()) {
            state = State::IDLE;
            speech.clear(); preroll.clear();
            onsetCount = trailCount = 0;
            if (useWake) wakeBuf.clear();
            usleep(5000);
            continue;
        }

        // ── Wake-word pass (runs always when configured, regardless of VAD) ──
        if (useWake) {
            wakeBuf.insert(wakeBuf.end(), frame.begin(), frame.end());
            while ((int)wakeBuf.size() >= WAKE_CHUNK_SAMPLES) {
                if (wakeWord_->processChunk(wakeBuf.data(), WAKE_CHUNK_SAMPLES)) {
                    Logger::info("Wake: detected (p=" +
                                 std::to_string(wakeWord_->lastProbability()) + ").");
                    armAwake();
                    // Reset VAD state so the wake utterance tail doesn't count
                    // as command speech.
                    state = State::IDLE;
                    speech.clear(); preroll.clear();
                    onsetCount = trailCount = 0;
                    wakeWord_->reset();
                    if (onWake_) onWake_();
                }
                wakeBuf.erase(wakeBuf.begin(), wakeBuf.begin() + WAKE_CHUNK_SAMPLES);
            }
        }

        const bool awakeNow = isAwake();
        if (!wasAwake && awakeNow)
            Logger::info("Recorder: awake window opened.");
        else if (wasAwake && !awakeNow)
            Logger::info("Recorder: awake window closed; re-arming wake gate.");
        wasAwake = awakeNow;

        // WebRTC VAD decision — 1=voice, 0=silence, -1=error
        int voiced = fvad_process(vad, frame.data(), FRAME_SAMPLES);
        if (voiced < 0) voiced = 0;

        switch (state) {
        case State::IDLE:
            preroll.push_back(frame);
            if ((int)preroll.size() > PREROLL) preroll.pop_front();
            if (voiced && awakeNow) { state = State::ONSET; onsetCount = 1; }
            break;

        case State::ONSET:
            preroll.push_back(frame);
            if ((int)preroll.size() > PREROLL) preroll.pop_front();
            if (voiced) {
                if (++onsetCount >= ONSET_FRAMES) {
                    state = State::SPEECH;
                    speech.clear();
                    for (auto& pr : preroll)
                        speech.insert(speech.end(), pr.begin(), pr.end());
                    preroll.clear();
                    Logger::info("VAD: speech detected.");
                }
            } else { state = State::IDLE; onsetCount = 0; }
            break;

        case State::SPEECH:
            speech.insert(speech.end(), frame.begin(), frame.end());
            if (!voiced) { state = State::TRAIL; trailCount = 1; }
            if ((int)speech.size() / FRAME_SAMPLES >= MAX_FRAMES) {
                Logger::warn("VAD: 20s cap, processing.");
                writeWav(speech);
                if (awakeNow) {
                    onSpeech_(outputPath_);
                    // extend the window — user is still talking
                    armAwake();
                } else {
                    Logger::info("VAD: (wake gate) dropped 20s segment.");
                }
                speech.clear(); state = State::IDLE;
            }
            break;

        case State::TRAIL:
            speech.insert(speech.end(), frame.begin(), frame.end());
            if (voiced) { state = State::SPEECH; trailCount = 0; }
            else if (++trailCount >= TRAIL_FRAMES) {
                if ((int)speech.size() / FRAME_SAMPLES >= MIN_FRAMES) {
                    if (awakeNow) {
                        Logger::info("VAD: speech end.");
                        writeWav(speech);
                        onSpeech_(outputPath_);
                        armAwake();  // user spoke — keep the window alive
                    } else {
                        Logger::info("VAD: (wake gate) dropped segment.");
                    }
                } else {
                    Logger::info("VAD: too short, skipped.");
                }
                speech.clear(); preroll.clear();
                state = State::IDLE; trailCount = 0;
            }
            break;
        }
    }

    pclose(pipe);
    fvad_free(vad);
}
