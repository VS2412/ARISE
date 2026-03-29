#include "recorder.hpp"
#include "logger.hpp"
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
    Logger::info("Recorder: WebRTC VAD started.");
}

void Recorder::stop() {
    active_.store(false);
    if (thread_.joinable()) thread_.join();
}

void Recorder::mute()   { muted_.store(true); }
void Recorder::unmute() { muted_.store(false); }

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
    fvad_set_mode(vad, 3);          // mode 3 = most aggressive, best for noise
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

    while (active_.load()) {
        size_t got = fread(frame.data(), sizeof(int16_t), FRAME_SAMPLES, pipe);
        if (got < static_cast<size_t>(FRAME_SAMPLES)) break;

        if (muted_.load()) {
            state = State::IDLE;
            speech.clear(); preroll.clear();
            onsetCount = trailCount = 0;
            usleep(5000);
            continue;
        }

        // WebRTC VAD decision — 1=voice, 0=silence, -1=error
        int voiced = fvad_process(vad, frame.data(), FRAME_SAMPLES);
        if (voiced < 0) voiced = 0;

        switch (state) {
        case State::IDLE:
            preroll.push_back(frame);
            if ((int)preroll.size() > PREROLL) preroll.pop_front();
            if (voiced) { state = State::ONSET; onsetCount = 1; }
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
                onSpeech_(outputPath_);
                speech.clear(); state = State::IDLE;
            }
            break;

        case State::TRAIL:
            speech.insert(speech.end(), frame.begin(), frame.end());
            if (voiced) { state = State::SPEECH; trailCount = 0; }
            else if (++trailCount >= TRAIL_FRAMES) {
                if ((int)speech.size() / FRAME_SAMPLES >= MIN_FRAMES) {
                    Logger::info("VAD: speech end.");
                    writeWav(speech);
                    onSpeech_(outputPath_);
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