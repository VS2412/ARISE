#include "daemon.hpp"
#include "logger.hpp"
#include "recorder.hpp"
#include "transcriber.hpp"
#include "llm.hpp"
#include "intent.hpp"
#include "executor.hpp"
#include "tts.hpp"
#include "context.hpp"
#include "memory.hpp"
#include "timer.hpp"
#include <thread>
#include <chrono>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <sstream>
#include <ctime>
#include <optional>

Daemon::Daemon(std::atomic<bool>& flag, std::atomic<bool>& pause)
    : shutdownRequested(flag), pauseToggle(pause) {}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool isInterruptCommand(const std::string& text) {
    auto t = lower(text);
    return t.find("stop")     != std::string::npos ||
           t.find("cancel")   != std::string::npos ||
           t.find("shut up")  != std::string::npos ||
           t.find("quiet")    != std::string::npos ||
           t.find("enough")   != std::string::npos;
}

// Wake-word detection. Whisper often mis-hears "aria" as "area", "ariah",
// "arya", "ariya" — so we accept a small family. Returns the suffix AFTER
// the wake phrase (possibly empty if user just said the name).
// nullopt means no wake phrase was found.
static std::optional<std::string> stripWakeWord(const std::string& text) {
    static const std::regex wakeRe(
        R"(^\s*(?:hey[,\s]+|ok[,\s]+|okay[,\s]+)?(?:aria|arya|ariah|ariya|area)[,\s.!?:-]*)",
        std::regex::icase);
    std::smatch m;
    if (!std::regex_search(text, m, wakeRe)) return std::nullopt;
    // Require the match to start at position 0 (with only leading whitespace)
    if (m.position(0) != 0) return std::nullopt;
    std::string rest = text.substr(m.position(0) + m.length(0));
    // Trim
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.erase(rest.begin());
    while (!rest.empty() && (rest.back()  == ' ' || rest.back()  == '\t')) rest.pop_back();
    return rest;
}

// count real words (skip punctuation-only tokens)
static int wordCount(const std::string& text) {
    int count = 0;
    bool inWord = false;
    for (char c : text) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            if (!inWord) { ++count; inWord = true; }
        } else {
            inWord = false;
        }
    }
    return count;
}

// Passive fact extraction: scan user speech for self-identifying phrases.
// Returns a map of fact_key → value. Only emits keys when a clean match is found.
static std::map<std::string, std::string> extractFacts(const std::string& text) {
    auto t = lower(text);
    std::map<std::string, std::string> facts;

    auto tryExtract = [&](const std::string& prefix) -> std::string {
        auto pos = t.find(prefix);
        if (pos == std::string::npos) return "";
        std::string rest = text.substr(pos + prefix.size());
        auto end = rest.find_first_of(".,!?;\n");
        if (end != std::string::npos) rest = rest.substr(0, end);
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.erase(rest.begin());
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();
        return rest;
    };

    auto firstMatch = [&](std::initializer_list<const char*> prefixes) -> std::string {
        for (auto p : prefixes) {
            auto v = tryExtract(p);
            if (!v.empty()) return v;
        }
        return "";
    };

    // Name (order matters — "my name is" is cleanest)
    auto name = firstMatch({"my name is ", "call me ", "i'm called ", "i am called "});
    if (!name.empty() && name.size() < 40) facts["user_name"] = name;

    // Location
    auto loc = firstMatch({"i live in ", "i am from ", "i'm from ", "i'm in ", "i am in "});
    if (!loc.empty() && loc.size() < 50) facts["location"] = loc;

    // Job / role
    auto job = firstMatch({"i work as ", "i work at ", "my job is "});
    if (!job.empty() && job.size() < 80) facts["job"] = job;

    // Preference
    auto pref = firstMatch({"i prefer ", "i like using ", "i swear by ", "i love ", "i really like "});
    if (!pref.empty() && pref.size() < 80) facts["preference"] = pref;

    // Current project
    auto proj = firstMatch({"i'm working on ", "i am working on ", "my project is ", "i'm building "});
    if (!proj.empty() && proj.size() < 120) facts["current_project"] = proj;

    // Favorite <topic> is <value> — captures topic dynamically as favorite_<topic>
    {
        std::regex favRe(R"(my favou?rite (\w+(?:\s+\w+)?) (?:is|are) ([^.,!?;\n]+))",
                         std::regex::icase);
        std::smatch m;
        if (std::regex_search(text, m, favRe)) {
            std::string topic = m[1].str();
            std::string val   = m[2].str();
            std::transform(topic.begin(), topic.end(), topic.begin(), ::tolower);
            for (auto& c : topic) if (c == ' ') c = '_';
            while (!val.empty() && (val.back()  == ' ' || val.back()  == '\t')) val.pop_back();
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
            if (!val.empty() && val.size() < 80)
                facts["favorite_" + topic] = val;
        }
    }

    return facts;
}

// Phase 5: detect user mood from transcribed speech.
// Lightweight regex-based classifier — returns "" (neutral), "frustrated",
// "urgent", or "curious". Feeds the LLM system prompt so replies adapt.
static std::string detectTone(const std::string& text) {
    auto t = lower(text);

    // Frustration markers — profanity-adjacent, repeated "why", "not working"
    static const std::regex frustRe(
        R"((damn|dammit|shit|crap|wtf|ugh|come on|why isn'?t|not working|broken|still not|again\?|seriously|whatever))",
        std::regex::icase);
    // Urgency — "now", "quick", "fast", "asap", "hurry"
    static const std::regex urgRe(
        R"(\b(now|asap|quickly|quick|fast|hurry|right now|immediately)\b)",
        std::regex::icase);
    // Curiosity — "how does", "what is", "why does", "explain", "tell me about"
    static const std::regex curRe(
        R"(\b(how does|how do|what is|why does|why is|explain|tell me about|curious|wonder(?:ing)?)\b)",
        std::regex::icase);

    if (std::regex_search(t, frustRe)) return "frustrated";
    if (std::regex_search(t, urgRe))   return "urgent";
    if (std::regex_search(t, curRe))   return "curious";
    return "";
}

// Time-of-day bucket from local wall clock.
static std::string currentTimeOfDay() {
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    int h = lt.tm_hour;
    if (h < 5)  return "night";
    if (h < 12) return "morning";
    if (h < 17) return "afternoon";
    if (h < 21) return "evening";
    return "night";
}

// "Thursday, April 15" — concise date label for the system prompt.
static std::string currentDateLabel() {
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%A, %B %e", &lt) == 0) return "";
    return buf;
}

// Helper: get a string from JSON args
static std::string arg(const nlohmann::json& a, const std::string& key) {
    if (a.contains(key) && a[key].is_string()) return a[key].get<std::string>();
    if (a.contains("param") && a["param"].is_string()) return a["param"].get<std::string>();
    return "";
}

void Daemon::run() {
    Logger::info("Initializing pipeline...");

    const char* home = std::getenv("HOME");
    std::string dbPath = home ? std::string(home) + "/.aria_memory.db" : "/tmp/aria_memory.db";

    Transcriber transcriber("/home/Aurelius/Documents/AdoVs/whisper.cpp/models/ggml-small.en.bin");
    const char* modelEnv = std::getenv("ARIA_MODEL");
    LLM         llm(modelEnv ? modelEnv : "qwen3:8b");
    Executor    executor;
    TTS         tts("/home/Aurelius/.local/share/piper/en_US-lessac-medium.onnx");
    Memory      memory(dbPath);

    std::mutex              qMutex;
    std::queue<std::string> speechQueue;
    std::condition_variable qCV;
    std::atomic<bool>       ariaActive{true};

    Recorder recorder("/tmp/aria_speech.wav",
        [&](const std::string& wavPath) {
            std::lock_guard<std::mutex> lock(qMutex);
            speechQueue.push(wavPath);
            qCV.notify_one();
        });

    TimerManager timers([&tts, &recorder](const std::string& label) {
        recorder.mute();
        tts.speak("Timer done: " + label);
        system(("notify-send 'ARIA Timer' " + std::string("'") + label + "'").c_str());
        recorder.unmute();
    });

    // Handle timer actions that the executor delegates back to us
    auto handleTimerAction = [&](const AgentAction& act) -> std::string {
        if (act.type == "timer_set") {
            int secs = 0;
            if (act.args.contains("duration_seconds") && act.args["duration_seconds"].is_number())
                secs = act.args["duration_seconds"].get<int>();
            std::string label = arg(act.args, "label");
            if (secs <= 0) return "Invalid duration.";
            int id = timers.addTimer(secs, label);
            return "Timer #" + std::to_string(id) + " set for " + std::to_string(secs) + " seconds.";
        }
        if (act.type == "timer_list") {
            return timers.list();
        }
        if (act.type == "timer_cancel") {
            int id = 0;
            if (act.args.contains("id") && act.args["id"].is_number())
                id = act.args["id"].get<int>();
            return timers.cancel(id) ? "Timer cancelled." : "Timer not found.";
        }
        return "";
    };

    // Execute an action, routing timers, memory recall, and clarification asks
    // to internal handlers. ask_user just speaks the question — the next user
    // utterance continues via LLM history, no ReAct hop needed.
    auto executeAction = [&](const AgentAction& act) -> std::string {
        if (act.type == "timer_set" || act.type == "timer_list" || act.type == "timer_cancel")
            return handleTimerAction(act);
        if (act.type == "recall_memory") {
            std::string query = arg(act.args, "query");
            if (query.empty()) return "No query specified.";
            auto results = memory.search(query, 5);
            if (results.empty()) return "Nothing found in memory about that.";
            std::ostringstream out;
            for (auto& r : results)
                out << "[" << r.timestamp << "] " << r.role << ": " << r.content << "\n";
            return out.str();
        }
        if (act.type == "ask") {
            std::string q = arg(act.args, "question");
            return q.empty() ? "What should I do?" : q;
        }
        return executor.execute(act);
    };

    // Background summarizer: every 20 turns, compress old conversation into a summary.
    // Runs detached so it doesn't block the user.
    auto maybeSummarize = [&]() {
        int lastId   = memory.lastConversationId();
        int lastSumm = memory.lastSummarizedId();
        if (lastId - lastSumm < 20) return;
        std::thread([&memory, &llm, lastSumm, lastId]() {
            auto turns = memory.getRange(lastSumm, lastId);
            if (turns.size() < 10) return;
            std::ostringstream text;
            for (auto& t : turns)
                text << t.role << ": " << t.content << "\n";
            auto summary = llm.summarize(text.str());
            if (!summary.empty()) {
                memory.saveSummary(summary, lastSumm, lastId);
                Logger::info("Memory: summarized turns " +
                             std::to_string(lastSumm + 1) + "-" + std::to_string(lastId));
            }
        }).detach();
    };

    std::string lastFactKey_;
    auto lastFactTime_ = std::chrono::steady_clock::now();

    // Phase 4: wake-word mode. When ARIA_WAKE_WORD is set, ARIA only responds
    // if the utterance begins with the wake phrase. After a successful
    // activation, a 5-second follow-up window lets the user chain commands
    // without repeating "Aria".
    const char* wakeEnv = std::getenv("ARIA_WAKE_WORD");
    const bool wakeRequired = (wakeEnv && wakeEnv[0] != '\0' && wakeEnv[0] != '0');
    std::chrono::steady_clock::time_point awakeUntil{};
    constexpr auto kFollowUpWindow = std::chrono::seconds(5);
    if (wakeRequired)
        Logger::info("Wake-word mode enabled (say 'Aria' to activate).");

    std::thread processor([&]() {
    while (!shutdownRequested.load()) {
        std::unique_lock<std::mutex> lock(qMutex);
        qCV.wait_for(lock, std::chrono::milliseconds(100),
            [&]{ return !speechQueue.empty() || shutdownRequested.load(); });
        if (speechQueue.empty()) continue;
        std::string wavPath = speechQueue.front();
        speechQueue.pop();
        lock.unlock();

        if (!ariaActive.load()) continue;

        std::string text = transcriber.transcribe(wavPath);
        if (text.empty() || text.find("[BLANK_AUDIO]") != std::string::npos) {
            Logger::info("VAD: blank, skipping.");
            continue;
        }
        Logger::info("You said: " + text);

        if (isInterruptCommand(text)) {
            tts.interrupt();
            Logger::info("ARIA: interrupted.");
            continue;
        }

        // ─── Wake-word gate ───
        // ALWAYS mode: skip entirely. WAKE_WORD mode: require wake phrase
        // unless we're still inside the 5s follow-up window.
        if (wakeRequired) {
            auto now = std::chrono::steady_clock::now();
            bool awake = (awakeUntil > now);
            auto stripped = stripWakeWord(text);
            if (stripped.has_value()) {
                // Wake phrase present — strip it and activate.
                text = *stripped;
                awakeUntil = now + kFollowUpWindow;
                if (text.empty()) {
                    // User said just "Aria" / "Hey Aria" — acknowledge and wait.
                    recorder.mute();
                    tts.speak("Yes?");
                    recorder.unmute();
                    Logger::info("Wake: acknowledged, awaiting follow-up.");
                    continue;
                }
                Logger::info("Wake: activated → " + text);
            } else if (awake) {
                // Inside follow-up window — accept utterance, extend window.
                awakeUntil = now + kFollowUpWindow;
                Logger::info("Wake: follow-up accepted.");
            } else {
                Logger::info("Wake: asleep, ignored: " + text);
                continue;
            }
        }

        // Passive fact extraction — learn name, location, job, preferences, projects
        auto newFacts = extractFacts(text);
        for (auto& [k, v] : newFacts) {
            memory.setFact(k, v);
            Logger::info("Memory: learned " + k + " = " + v);
            lastFactKey_ = k;
        }

        // Correction path: if user immediately follows a fact with
        // "actually X" / "it's X" / "I meant X" / "no it's X", overwrite the
        // most recently set fact with the corrected value. Scoped to the
        // last ~2 minutes to avoid spurious rewrites later in the session.
        if (newFacts.empty() && !lastFactKey_.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto ageSec = std::chrono::duration_cast<std::chrono::seconds>(
                              now - lastFactTime_).count();
            if (ageSec <= 120) {
                std::regex corrRe(
                    R"(^\s*(?:-\s*)?(?:actually|sorry|no[, ]|i meant|i mean|it'?s|it is)\s+([A-Za-z0-9][A-Za-z0-9 .\-+_]{0,60}?)\s*[.!?]?\s*$)",
                    std::regex::icase);
                std::smatch m;
                if (std::regex_search(text, m, corrRe)) {
                    std::string v = m[1].str();
                    while (!v.empty() && (v.back()  == ' ' || v.back()  == '\t')) v.pop_back();
                    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                    if (!v.empty() && v.size() < 80) {
                        memory.setFact(lastFactKey_, v);
                        Logger::info("Memory: corrected " + lastFactKey_ + " → " + v);
                    }
                }
            }
        }
        if (!newFacts.empty())
            lastFactTime_ = std::chrono::steady_clock::now();

        // INTENT CLASSIFIER FIRST — runs before word count filter
        // so "open firefox" (2 words) still works
        AgentAction directAction = classifyIntent(text);

        // skip noise ONLY if intent didn't match — short known commands are fine
        if (directAction.empty() && wordCount(text) <= 2) {
            Logger::info("Skipped short utterance: " + text);
            continue;
        }

        recorder.mute();

        if (!directAction.empty()) {
            // known command — execute instantly
            Logger::info("Intent: " + directAction.type + " → " + directAction.args.dump());
            std::string feedback = executeAction(directAction);
            memory.save("user", text);
            memory.save("assistant", directAction.type + ":" + directAction.args.dump());
            if (!feedback.empty()) tts.speak(feedback);
        } else {
            // unknown — send to LLM for reasoning
            auto sysCtx = Context::capture();
            Logger::info("Context: app=" + sysCtx.activeApp + " window=" + sysCtx.activeWindow);

            // answer simple context queries directly — no LLM needed
            auto lt = lower(text);
            std::string directAnswer;
            if ((lt.find("what app") != std::string::npos ||
                 lt.find("which app") != std::string::npos) &&
                !sysCtx.activeApp.empty()) {
                directAnswer = "You're using " + sysCtx.activeApp + ".";
            } else if ((lt.find("what's in my clipboard") != std::string::npos ||
                        lt.find("what did i copy") != std::string::npos ||
                        lt.find("read clipboard") != std::string::npos) &&
                       !sysCtx.clipboard.empty()) {
                directAnswer = "Your clipboard says: " + sysCtx.clipboard;
            } else if (lt.find("what window") != std::string::npos &&
                       !sysCtx.activeWindow.empty()) {
                directAnswer = "The window title is " + sysCtx.activeWindow + ".";
            }
            if (!directAnswer.empty()) {
                Logger::info("Context query → " + directAnswer);
                memory.save("user", text);
                memory.save("assistant", directAnswer);
                tts.speak(directAnswer);
                recorder.unmute();
                maybeSummarize();
                continue;
            }

            LLMContext ctx;
            ctx.activeApp     = sysCtx.activeApp;
            ctx.activeWindow  = sysCtx.activeWindow;
            ctx.clipboard     = sysCtx.clipboard;
            ctx.screenText    = sysCtx.screenText;
            ctx.memorySummary = memory.getSummary();
            ctx.tone          = detectTone(text);
            ctx.timeOfDay     = currentTimeOfDay();
            ctx.dateLabel     = currentDateLabel();
            if (!ctx.tone.empty())
                Logger::info("Tone: " + ctx.tone);

            auto userName = memory.getFact("user_name");
            std::string prompt = text;
            if (!userName.empty() &&
                (lower(text).find("hello") != std::string::npos ||
                 lower(text).find("hey")   != std::string::npos))
                prompt = text + " (user's name is " + userName + ")";

            // ─── Streaming LLM → Streaming TTS ───
            tts.startStream();
            auto response = llm.thinkStreaming(prompt, ctx,
                [&tts](const std::string& delta) {
                    tts.feedChunk(delta);
                });

            memory.save("user", text);

            if (response.hasAction()) {
                // Tool calls arrived — end the text stream (may be empty)
                tts.endStream();

                // Check if any action is a "run" command (needs observation feedback)
                bool hasRunAction = false;
                for (auto& a : response.actions)
                    if (a.type == "run") hasRunAction = true;

                auto lt2 = lower(text);
                bool isMultiStep = (lt2.find(" and ") != std::string::npos ||
                                    lt2.find(" then ") != std::string::npos ||
                                    lt2.find("set up") != std::string::npos ||
                                    lt2.find("setup")  != std::string::npos);
                bool useReact = hasRunAction || isMultiStep;

                if (useReact) {
                    // ReAct loop — execute, observe, let LLM decide next step
                    constexpr int MAX_REACT_STEPS = 5;
                    int step = 0;

                    while (response.hasAction() && !response.done && step < MAX_REACT_STEPS) {
                        std::string observation;

                        for (auto& act : response.actions) {
                            if (act.type == "done") {
                                response.done = true;
                                std::string summary = arg(act.args, "summary");
                                Logger::info("ReAct: task complete — " + summary);
                                memory.save("assistant", "task_done:" + summary);
                                tts.speak(summary);
                                break;
                            }

                            Logger::info("ReAct step " + std::to_string(step + 1) +
                                         ": " + act.type + " → " + act.args.dump());

                            std::string result;
                            if (act.type == "run") {
                                // Route through the safety-guarded capture so ReAct-driven
                                // shell commands get the same deny list as the non-ReAct path.
                                result = executor.safeShellCapture(arg(act.args, "command"));
                            } else {
                                result = executeAction(act);
                            }
                            memory.save("assistant", act.type + ":" + act.args.dump());

                            if (!observation.empty()) observation += "\n";
                            observation += result;
                        }

                        if (response.done) break;

                        if (!response.speech.empty())
                            tts.speak(response.speech);

                        step++;
                        if (step >= MAX_REACT_STEPS) {
                            Logger::warn("ReAct: hit max steps (" + std::to_string(MAX_REACT_STEPS) + ")");
                            tts.speak("Done after " + std::to_string(step) + " steps.");
                            break;
                        }

                        auto freshCtx = Context::capture();
                        ctx.activeApp    = freshCtx.activeApp;
                        ctx.activeWindow = freshCtx.activeWindow;
                        ctx.clipboard    = freshCtx.clipboard;
                        ctx.screenText   = freshCtx.screenText;

                        // Stream react response too
                        tts.startStream();
                        response = llm.reactStreaming(observation, ctx,
                            [&tts](const std::string& delta) {
                                tts.feedChunk(delta);
                            });
                        tts.endStream();
                    }
                } else {
                    // Simple action(s) — execute directly, no ReAct
                    for (auto& act : response.actions) {
                        Logger::info("LLM action: " + act.type + " → " + act.args.dump());
                        std::string feedback = executeAction(act);
                        memory.save("assistant", act.type + ":" + act.args.dump());
                        if (response.speech.empty() && !feedback.empty())
                            tts.speak(feedback);
                    }
                }

                // Speak any speech that came with tool calls
                if (!response.speech.empty() && response.hasAction()) {
                    memory.save("assistant", response.speech);
                    tts.speak(response.speech);
                }
            } else {
                // Pure conversational response — stream already fed to TTS
                tts.endStream();
                if (!response.speech.empty())
                    memory.save("assistant", response.speech);
            }
        }

        recorder.unmute();
        maybeSummarize();
    }
});

    recorder.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // startup greeting with name if known
    recorder.mute();
    auto userName = memory.getFact("user_name");
    tts.speak(userName.empty() ? "Online." : "Online, " + userName + ".");
    recorder.unmute();

    Logger::info("ARIA ready.");

    while (!shutdownRequested.load()) {
        if (pauseToggle.exchange(false)) {
            bool nowActive = !ariaActive.load();
            ariaActive.store(nowActive);
            recorder.mute();
            tts.speak(nowActive ? "Back." : "Pausing.");
            if (nowActive) recorder.unmute();
            Logger::info(nowActive ? "ARIA: resumed." : "ARIA: paused.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    recorder.stop();
    processor.join();
    Logger::info("Daemon shutting down cleanly.");
}
