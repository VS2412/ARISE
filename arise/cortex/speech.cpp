#include "cortex/speech.hpp"

#include "util/log.hpp"

#include <cctype>
#include <thread>
#include <unordered_set>

namespace arise {

namespace {

// Lower-case ASCII helper.
std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Heuristic: is the trailing token of `s` (everything before position `dot`)
// an abbreviation that shouldn't end a sentence? "Dr.", "Mr.", "etc." …
bool endsWithAbbreviation(std::string_view s, std::size_t dot_pos) {
    static const std::unordered_set<std::string> kAbbrev = {
        "mr", "mrs", "ms", "dr", "st", "prof", "sr", "jr",
        "etc", "vs", "e.g", "i.e", "fig", "no", "ph", "rev",
        "ave", "blvd", "rd", "ln",
        // Time/date markers.
        "a.m", "p.m",
    };
    // Walk back from dot_pos for [a-zA-Z.] characters.
    std::size_t i = dot_pos;
    while (i > 0) {
        char c = s[i-1];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '.') --i;
        else break;
    }
    auto tok = std::string(s.substr(i, dot_pos - i));
    if (tok.empty()) return false;
    return kAbbrev.count(toLower(tok)) > 0;
}

// Heuristic: is the dot a decimal point ("3.14")? Both sides digits.
bool isDecimalDot(std::string_view s, std::size_t dot_pos) {
    if (dot_pos == 0 || dot_pos + 1 >= s.size()) return false;
    return std::isdigit(static_cast<unsigned char>(s[dot_pos - 1]))
        && std::isdigit(static_cast<unsigned char>(s[dot_pos + 1]));
}

// Trim leading + trailing whitespace.
std::string trim(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j-1]))) --j;
    return std::string(s.substr(i, j - i));
}

} // namespace

// ─── mood → params ─────────────────────────────────────────────────────────

TtsParams moodToParams(std::string_view mood_label) {
    TtsParams p;
    p.mood_label = std::string(mood_label);
    auto m = toLower(mood_label);

    // Defaults match piper's documented values.
    p.length_scale         = 1.00;
    p.noise_scale          = 0.667;
    p.noise_w              = 0.80;
    p.sentence_silence_sec = 0.20;

    if (m == "tired" || m == "empathetic") {
        p.length_scale         = 1.18;
        p.noise_scale          = 0.55;
        p.sentence_silence_sec = 0.35;
    } else if (m == "frustrated" || m == "down") {
        // Soften delivery rather than match the user's energy.
        p.length_scale         = 1.10;
        p.noise_scale          = 0.55;
        p.sentence_silence_sec = 0.30;
    } else if (m == "warm") {
        p.length_scale         = 1.05;
        p.noise_scale          = 0.70;
        p.sentence_silence_sec = 0.25;
    } else if (m == "focused" || m == "alert") {
        p.length_scale         = 0.95;
        p.noise_scale          = 0.60;
        p.sentence_silence_sec = 0.15;
    } else if (m == "excited") {
        p.length_scale         = 0.88;
        p.noise_scale          = 0.75;
        p.sentence_silence_sec = 0.15;
    }
    return p;
}

// ─── sentence chunker ──────────────────────────────────────────────────────

std::vector<Sentence> chunkSentences(std::string_view text,
                                     const TtsParams& p) {
    std::vector<Sentence> out;
    if (text.empty()) return out;

    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        bool is_term = (c == '.' || c == '!' || c == '?');
        if (!is_term) continue;

        if (c == '.' && (endsWithAbbreviation(text, i) || isDecimalDot(text, i))) {
            continue;
        }

        // Walk forward through any consecutive terminators (e.g. "...", "!?")
        std::size_t end = i;
        while (end + 1 < text.size()
               && (text[end+1] == '.' || text[end+1] == '!' || text[end+1] == '?')) {
            ++end;
        }
        // Optional trailing closing quote / paren / bracket.
        while (end + 1 < text.size()
               && (text[end+1] == '"' || text[end+1] == '\''
                   || text[end+1] == ')' || text[end+1] == ']')) {
            ++end;
        }

        auto piece = trim(text.substr(start, end - start + 1));
        if (!piece.empty()) {
            Sentence s;
            s.text          = std::move(piece);
            s.post_pause_ms = int(p.sentence_silence_sec * 1000.0);
            out.push_back(std::move(s));
        }
        start = end + 1;
        i     = end;
    }
    if (start < text.size()) {
        auto tail = trim(text.substr(start));
        if (!tail.empty()) {
            Sentence s;
            s.text          = std::move(tail);
            s.post_pause_ms = int(p.sentence_silence_sec * 1000.0);
            out.push_back(std::move(s));
        }
    }
    return out;
}

// ─── Speech facade ─────────────────────────────────────────────────────────

Speech::Speech(Config cfg) : cfg_(std::move(cfg)) {
    if (!cfg_.primary) log::error("Speech: primary engine is required");
}

Speech::Stats Speech::say(std::string_view text,
                          std::string_view mood_label,
                          const TtsParams* override) {
    Stats stats;
    if (!cfg_.primary || text.empty()) return stats;

    TtsParams p = override ? *override : moodToParams(mood_label);
    auto sentences = chunkSentences(text, p);
    if (sentences.empty()) return stats;

    auto try_engine = [&](TtsEngine* eng, std::string_view body) -> TtsEngine::Result {
        if (!eng) {
            TtsEngine::Result r; r.error = "no engine"; return r;
        }
        return eng->speak(body, p);
    };

    for (const auto& sent : sentences) {
        ++stats.sentences;
        TtsEngine::Result r = try_engine(cfg_.primary, sent.text);
        if (r.ok) {
            stats.engine_used = cfg_.primary->name();
        } else if (cfg_.fallback) {
            log::warn(std::string("Speech: primary failed (")
                      + r.error + "); trying fallback");
            r = try_engine(cfg_.fallback, sent.text);
            if (r.ok) stats.engine_used = cfg_.fallback->name();
        }

        if (!r.ok) ++stats.sentences_failed;
        stats.bytes_total += r.bytes_synthesized;
        stats.duration_ms += r.duration_ms;

        if (cfg_.inter_sentence_pause.count() > 0
            && stats.sentences < int(sentences.size())) {
            std::this_thread::sleep_for(cfg_.inter_sentence_pause);
        }
    }
    return stats;
}

} // namespace arise
