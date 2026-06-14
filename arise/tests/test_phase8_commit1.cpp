// Phase 8 commit 1: Speech facade + sentence chunker + mood→params +
// persona prompt + Piper engine availability probe.
//
// All tests are deterministic and don't need audio. Live audio is exercised
// manually via `arise speak`.

#include "cortex/identity.hpp"
#include "cortex/memory_cortex.hpp"
#include "cortex/persona_prompt.hpp"
#include "cortex/piper_engine.hpp"
#include "cortex/speech.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace {

// Minimal in-process engine that records every call. Used to drive the
// Speech facade end-to-end without any real TTS.
class FakeEngine : public arise::TtsEngine {
public:
    struct Call {
        std::string text;
        arise::TtsParams params;
    };

    Result speak(std::string_view text, const arise::TtsParams& p) override {
        std::lock_guard<std::mutex> lk(mu_);
        Call c; c.text = std::string(text); c.params = p;
        calls_.push_back(std::move(c));
        Result r;
        r.ok = !fail_;
        r.bytes_synthesized = text.size() * 2;
        r.duration_ms = 1;
        if (fail_) r.error = "fake_engine forced failure";
        return r;
    }

    bool        isAvailable() const override { return !fail_; }
    std::string name() const override { return name_; }

    void setName(std::string n)  { name_ = std::move(n); }
    void setFail(bool b)         { fail_ = b; }

    std::vector<Call> calls() const {
        std::lock_guard<std::mutex> lk(mu_);
        return calls_;
    }

private:
    mutable std::mutex mu_;
    std::vector<Call>  calls_;
    bool               fail_ = false;
    std::string        name_ = "fake";
};

} // namespace

// ─── moodToParams ──────────────────────────────────────────────────────────

TEST(MoodToParams, KnownLabelsCarryDistinctSettings) {
    auto neutral = arise::moodToParams("neutral");
    auto excited = arise::moodToParams("excited");
    auto tired   = arise::moodToParams("tired");
    auto frust   = arise::moodToParams("frustrated");

    EXPECT_EQ(neutral.mood_label, "neutral");
    EXPECT_GT(tired.length_scale,   neutral.length_scale);
    EXPECT_LT(excited.length_scale, neutral.length_scale);
    EXPECT_GT(tired.sentence_silence_sec,   neutral.sentence_silence_sec);
    EXPECT_LT(excited.sentence_silence_sec, neutral.sentence_silence_sec);
    // Frustrated softens delivery — slower than neutral, not faster.
    EXPECT_GT(frust.length_scale, neutral.length_scale);
    EXPECT_LT(frust.noise_scale,  neutral.noise_scale);   // less variation
}

TEST(MoodToParams, UnknownLabelFallsBackToNeutral) {
    auto p = arise::moodToParams("garblefoo");
    EXPECT_EQ(p.length_scale,         1.00);
    EXPECT_NEAR(p.noise_scale,        0.667, 1e-6);
    EXPECT_EQ(p.sentence_silence_sec, 0.20);
}

// ─── chunker ───────────────────────────────────────────────────────────────

TEST(Chunker, EmptyAndWhitespaceOnly) {
    auto p = arise::moodToParams("neutral");
    EXPECT_TRUE(arise::chunkSentences("",     p).empty());
    EXPECT_TRUE(arise::chunkSentences("   ",  p).empty());
}

TEST(Chunker, SplitsOnTerminators) {
    auto p = arise::moodToParams("neutral");
    auto cs = arise::chunkSentences("Hi there. How are you? Great!", p);
    ASSERT_EQ(cs.size(), 3u);
    EXPECT_EQ(cs[0].text, "Hi there.");
    EXPECT_EQ(cs[1].text, "How are you?");
    EXPECT_EQ(cs[2].text, "Great!");
}

TEST(Chunker, KeepsAbbreviationsTogether) {
    auto p = arise::moodToParams("neutral");
    auto cs = arise::chunkSentences("Dr. Strange said hi. Then Mr. Smith left.", p);
    ASSERT_EQ(cs.size(), 2u);
    EXPECT_EQ(cs[0].text, "Dr. Strange said hi.");
    EXPECT_EQ(cs[1].text, "Then Mr. Smith left.");
}

TEST(Chunker, DecimalsAreNotSplit) {
    auto p = arise::moodToParams("neutral");
    auto cs = arise::chunkSentences("Pi is about 3.14 today. Cool right?", p);
    ASSERT_EQ(cs.size(), 2u);
    EXPECT_EQ(cs[0].text, "Pi is about 3.14 today.");
    EXPECT_EQ(cs[1].text, "Cool right?");
}

TEST(Chunker, CollapsesConsecutiveTerminators) {
    auto p = arise::moodToParams("neutral");
    auto cs = arise::chunkSentences("Wait... what?! No way!!!", p);
    ASSERT_EQ(cs.size(), 3u);
    EXPECT_EQ(cs[0].text, "Wait...");
    EXPECT_EQ(cs[1].text, "what?!");
    EXPECT_EQ(cs[2].text, "No way!!!");
}

TEST(Chunker, TrailingFragmentWithoutTerminator) {
    auto p = arise::moodToParams("neutral");
    auto cs = arise::chunkSentences("Hello there", p);
    ASSERT_EQ(cs.size(), 1u);
    EXPECT_EQ(cs[0].text, "Hello there");
}

TEST(Chunker, IncludesTrailingQuotesAndParens) {
    auto p = arise::moodToParams("neutral");
    auto cs = arise::chunkSentences("She said \"hi.\" He waved.", p);
    ASSERT_EQ(cs.size(), 2u);
    EXPECT_EQ(cs[0].text, "She said \"hi.\"");
}

TEST(Chunker, MoodAdjustsPostPause) {
    auto cs_neutral = arise::chunkSentences("a. b.", arise::moodToParams("neutral"));
    auto cs_tired   = arise::chunkSentences("a. b.", arise::moodToParams("tired"));
    ASSERT_EQ(cs_neutral.size(), 2u);
    ASSERT_EQ(cs_tired.size(),   2u);
    EXPECT_GT(cs_tired[0].post_pause_ms, cs_neutral[0].post_pause_ms);
}

// ─── Speech facade w/ FakeEngine ───────────────────────────────────────────

TEST(SpeechFacade, RoutesEverySentenceToPrimary) {
    FakeEngine eng;
    arise::Speech::Config sc; sc.primary = &eng;
    arise::Speech sp(sc);

    auto stats = sp.say("First. Second. Third.", "neutral");
    EXPECT_EQ(stats.sentences, 3);
    EXPECT_EQ(stats.sentences_failed, 0);
    EXPECT_EQ(stats.engine_used, "fake");
    auto calls = eng.calls();
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[0].text, "First.");
    EXPECT_EQ(calls[1].text, "Second.");
    EXPECT_EQ(calls[2].text, "Third.");
}

TEST(SpeechFacade, FallbackKicksInOnPrimaryFailure) {
    FakeEngine primary; primary.setFail(true); primary.setName("primary");
    FakeEngine fb;                              fb.setName("fb");
    arise::Speech::Config sc; sc.primary = &primary; sc.fallback = &fb;
    arise::Speech sp(sc);

    auto stats = sp.say("One. Two.", "neutral");
    EXPECT_EQ(stats.sentences, 2);
    EXPECT_EQ(stats.sentences_failed, 0);
    EXPECT_EQ(stats.engine_used, "fb");
    EXPECT_EQ(primary.calls().size(), 2u);
    EXPECT_EQ(fb.calls().size(),      2u);
}

TEST(SpeechFacade, ParamsOverrideWinsOverMoodLabel) {
    FakeEngine eng;
    arise::Speech::Config sc; sc.primary = &eng;
    arise::Speech sp(sc);

    arise::TtsParams override;
    override.mood_label         = "custom";
    override.length_scale       = 1.55;
    override.sentence_silence_sec = 0.05;
    sp.say("Hi.", "tired", &override);
    auto calls = eng.calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].params.mood_label, "custom");
    EXPECT_NEAR(calls[0].params.length_scale, 1.55, 1e-9);
    EXPECT_NEAR(calls[0].params.sentence_silence_sec, 0.05, 1e-9);
}

TEST(SpeechFacade, EmptyTextNoOps) {
    FakeEngine eng;
    arise::Speech::Config sc; sc.primary = &eng;
    arise::Speech sp(sc);
    auto stats = sp.say("", "neutral");
    EXPECT_EQ(stats.sentences, 0);
    EXPECT_TRUE(eng.calls().empty());
}

// ─── PiperEngine availability probe ────────────────────────────────────────

TEST(PiperEngineProbe, MissingBinaryReportsFalse) {
    arise::PiperEngine::Config c;
    c.piper_bin           = "definitely-not-real-binary-arise-test";
    c.default_model_path  = "/nope.onnx";
    arise::PiperEngine eng(c);
    EXPECT_FALSE(eng.isAvailable());
}

TEST(PiperEngineProbe, MissingModelReportsFalse) {
    arise::PiperEngine::Config c;
    c.piper_bin           = "/bin/sh";       // exists + executable, stand-in
    c.aplay_bin           = "/bin/sh";
    c.default_model_path  = "/nope.onnx";
    arise::PiperEngine eng(c);
    EXPECT_FALSE(eng.isAvailable());
}

TEST(PiperEngineSpeak, RejectsEmptyText) {
    arise::PiperEngine::Config c;
    c.piper_bin           = "/bin/sh";
    c.aplay_bin           = "/bin/sh";
    c.default_model_path  = "/dev/null";   // technically readable
    arise::PiperEngine eng(c);
    auto p = arise::moodToParams("neutral");
    auto r = eng.speak("", p);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("empty"), std::string::npos);
}

// ─── persona prompt builder ────────────────────────────────────────────────

TEST(PersonaPrompt, FullShapeIncludesAllSections) {
    arise::PersonaPromptInput in;
    in.identity.name             = "Aria";
    in.identity.pronouns         = "she/her";
    in.identity.persona_summary  = "Sharp, dry, helps Aurelius ship.";
    in.identity.do_list          = {"answer in under 3 sentences",
                                    "remember what worked"};
    in.identity.dont_list        = {"apologise unprompted"};
    in.user_name                 = "Aurelius";
    in.mood.valence              = -0.6;
    in.mood.arousal              =  0.5;

    auto out = arise::buildPersonaPrompt(in);
    EXPECT_NE(out.find("You are Aria"),                       std::string::npos);
    EXPECT_NE(out.find("(she/her)"),                          std::string::npos);
    EXPECT_NE(out.find("Aurelius"),                           std::string::npos);
    EXPECT_NE(out.find("Sharp, dry"),                         std::string::npos);
    EXPECT_NE(out.find("answer in under 3 sentences"),        std::string::npos);
    EXPECT_NE(out.find("apologise unprompted"),               std::string::npos);
    EXPECT_NE(out.find("frustrated"),                         std::string::npos);
}

TEST(PersonaPrompt, IsDeterministic) {
    arise::PersonaPromptInput in;
    in.identity.name = "ARISE";
    auto a = arise::buildPersonaPrompt(in);
    auto b = arise::buildPersonaPrompt(in);
    EXPECT_EQ(a, b);
}

TEST(PersonaPrompt, NoMoodLineWhenDisabled) {
    arise::PersonaPromptInput in;
    in.include_mood_line = false;
    in.mood.valence      = -0.7;
    auto out = arise::buildPersonaPrompt(in);
    EXPECT_EQ(out.find("Tone right now"), std::string::npos);
}

TEST(PersonaPrompt, NoDoDontWhenDisabled) {
    arise::PersonaPromptInput in;
    in.include_do_dont = false;
    in.identity.do_list   = {"a", "b"};
    in.identity.dont_list = {"x"};
    auto out = arise::buildPersonaPrompt(in);
    EXPECT_EQ(out.find("Do:"),    std::string::npos);
    EXPECT_EQ(out.find("Don't:"), std::string::npos);
}

TEST(PersonaPrompt, RespectsDoDontCaps) {
    arise::PersonaPromptInput in;
    in.identity.do_list = {"one","two","three","four","five","six"};
    in.max_do_items     = 2;
    auto out = arise::buildPersonaPrompt(in);
    EXPECT_NE(out.find("one"),  std::string::npos);
    EXPECT_NE(out.find("two"),  std::string::npos);
    EXPECT_EQ(out.find("three"), std::string::npos);
}

TEST(MoodToTtsLabel, MapsAxesToLabels) {
    arise::Mood m;
    m.valence = -0.6; m.arousal = 0.5;
    EXPECT_EQ(arise::moodToTtsLabel(m), "frustrated");
    m.valence = -0.6; m.arousal = 0.0;
    EXPECT_EQ(arise::moodToTtsLabel(m), "down");
    m.valence =  0.6; m.arousal = 0.5;
    EXPECT_EQ(arise::moodToTtsLabel(m), "excited");
    m.valence =  0.6; m.arousal = 0.0;
    EXPECT_EQ(arise::moodToTtsLabel(m), "warm");
    m.valence =  0.0; m.arousal = 0.5;
    EXPECT_EQ(arise::moodToTtsLabel(m), "alert");
    m.valence =  0.0; m.arousal = -0.6;
    EXPECT_EQ(arise::moodToTtsLabel(m), "tired");
    m.valence =  0.0; m.arousal =  0.0;
    EXPECT_EQ(arise::moodToTtsLabel(m), "neutral");
}
