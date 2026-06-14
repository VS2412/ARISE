#include "cortex/persona_prompt.hpp"

#include <sstream>

namespace arise {

std::string moodToTtsLabel(const Mood& m) {
    if (m.valence < -0.4 && m.arousal >  0.4) return "frustrated";
    if (m.valence < -0.4)                     return "down";
    if (m.valence >  0.4 && m.arousal >  0.4) return "excited";
    if (m.valence >  0.4)                     return "warm";
    if (m.arousal >  0.4)                     return "alert";
    if (m.arousal < -0.4)                     return "tired";
    return "neutral";
}

std::string buildToneDescriptor(const Mood& m) {
    auto label = moodToTtsLabel(m);
    if (label == "frustrated") return "frustrated — keep it gentle, no jokes";
    if (label == "down")       return "a touch down — soft and patient";
    if (label == "excited")    return "energised — bright but still concise";
    if (label == "warm")       return "warm and present";
    if (label == "alert")      return "alert and brisk";
    if (label == "tired")      return "tired — slow down, fewer words";
    return "neutral, even-keeled";
}

std::string buildPersonaPrompt(const PersonaPromptInput& in) {
    std::ostringstream os;

    auto name      = in.identity.name.empty() ? "ARISE" : in.identity.name;
    auto pronouns  = in.identity.pronouns.empty() ? "it/its" : in.identity.pronouns;

    os << "You are " << name << " (" << pronouns << ")";
    if (!in.user_name.empty()) {
        os << ", talking with " << in.user_name;
    }
    os << ".\n";

    if (!in.identity.persona_summary.empty()) {
        os << in.identity.persona_summary << "\n";
    } else {
        os << "Direct, warm, and remembers context across sessions.\n";
    }

    if (in.include_do_dont) {
        if (!in.identity.do_list.empty()) {
            os << "Do:\n";
            int n = 0;
            for (const auto& d : in.identity.do_list) {
                if (in.max_do_items > 0 && n >= in.max_do_items) break;
                if (d.empty()) continue;
                os << "  - " << d << "\n";
                ++n;
            }
        }
        if (!in.identity.dont_list.empty()) {
            os << "Don't:\n";
            int n = 0;
            for (const auto& d : in.identity.dont_list) {
                if (in.max_dont_items > 0 && n >= in.max_dont_items) break;
                if (d.empty()) continue;
                os << "  - " << d << "\n";
                ++n;
            }
        }
    }

    if (in.include_mood_line) {
        os << "Tone right now: " << buildToneDescriptor(in.mood) << ".\n";
    }
    return os.str();
}

} // namespace arise
