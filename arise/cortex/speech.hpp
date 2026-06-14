#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace arise {

// Audible parameters for one synthesis run, derived from a mood label and
// any explicit overrides. Engine implementations consume these directly.
struct TtsParams {
    std::string mood_label = "neutral";   // human-readable
    // Phoneme duration multiplier. >1 slows the speaker down, <1 speeds up.
    double      length_scale         = 1.0;
    // Generator noise. Higher = more variation / expressiveness; piper
    // ships at 0.667 by default — we leave that alone for "neutral".
    double      noise_scale          = 0.667;
    // Phoneme width noise. Slight movement keeps consecutive phonemes from
    // sounding mechanical when length_scale is high.
    double      noise_w              = 0.8;
    // Silence inserted by the engine after each sentence (seconds). Mood
    // pushes this up for "tired" / "empathetic", down for "focused".
    double      sentence_silence_sec = 0.20;
    // Override engine voice / model path. Empty = engine default.
    std::string voice;
};

// One sentence chunked out of a paragraph, ready to feed the engine. The
// trailing pause is post-engine — engines also do their own intra-sentence
// pause; this is for the gap *between* sentences when the engine streams.
struct Sentence {
    std::string text;
    int         post_pause_ms = 0;
};

// Sentence chunker. Splits on `.`, `!`, `?` while preserving common
// abbreviations ("Dr.", "Mr.", "etc.") and decimal numbers ("3.14").
// Empty input → empty vector.
std::vector<Sentence> chunkSentences(std::string_view text,
                                     const TtsParams& p);

// Pure mood-label → params mapping. Unknown labels → "neutral" defaults.
// Does NOT touch the system clock or any other state.
TtsParams moodToParams(std::string_view mood_label);

// Common engine interface. Concrete implementations: PiperEngine (commit 1),
// SesameEngine / KokoroEngine (commit 2 sidecar). Engines should be
// stateless — Speech may construct one per call when fallbacks are in play.
class TtsEngine {
public:
    struct Result {
        bool                ok            = false;
        std::size_t         bytes_synthesized = 0;
        int                 duration_ms   = 0;
        std::string         error;
    };

    virtual ~TtsEngine() = default;

    // Synthesise + (optionally) play `text` as one chunk. The Speech facade
    // calls this once per Sentence so engines don't need their own splitter.
    virtual Result speak(std::string_view text, const TtsParams& params) = 0;

    // Cheap probe: is the binary / endpoint available?
    virtual bool   isAvailable() const { return true; }

    virtual std::string name() const = 0;
};

// Speech facade. Holds a primary engine + optional emergency fallback (the
// plan calls out Piper as the always-works baseline). Owns mood resolution
// and sentence chunking; delegates synthesis.
class Speech {
public:
    struct Config {
        TtsEngine* primary  = nullptr;     // required
        TtsEngine* fallback = nullptr;     // optional; tried on primary failure
        // Inserted between sentences in addition to the engine's own
        // sentence_silence (which is *intra*-engine). 0 by default —
        // engine-side silence is usually enough.
        std::chrono::milliseconds inter_sentence_pause { 0 };
    };

    struct Stats {
        int            sentences        = 0;
        int            sentences_failed = 0;
        std::size_t    bytes_total      = 0;
        int            duration_ms      = 0;
        std::string    engine_used;       // last engine (primary or fallback)
    };

    explicit Speech(Config cfg);

    // Synthesise + speak `text` using `mood_label` (overridden by an
    // explicit `params_override`). Sentences are chunked and dispatched
    // sequentially. Returns aggregate Stats. Always synchronous; the
    // caller's thread is the audio thread.
    Stats say(std::string_view text,
              std::string_view mood_label = "neutral",
              const TtsParams* params_override = nullptr);

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

} // namespace arise
