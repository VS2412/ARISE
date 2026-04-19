#include "daemon.hpp"
#include "logger.hpp"
#include "config.hpp"
#include "recorder.hpp"
#include "wakeword.hpp"
#include "transcriber.hpp"
#include "llm.hpp"
#include "intent.hpp"
#include "executor.hpp"
#include "tts.hpp"
#include "context.hpp"
#include "memory.hpp"
#include "timer.hpp"
#include "vision.hpp"
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
#include <cstdio>
#include <optional>

Daemon::Daemon(std::atomic<bool>& flag, std::atomic<bool>& pause)
    : shutdownRequested(flag), pauseToggle(pause) {}

// ─── Static helpers (unchanged from v2) ───

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
    if (m.position(0) != 0) return std::nullopt;
    std::string rest = text.substr(m.position(0) + m.length(0));
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.erase(rest.begin());
    while (!rest.empty() && (rest.back()  == ' ' || rest.back()  == '\t')) rest.pop_back();
    return rest;
}

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

    auto name = firstMatch({"my name is ", "call me ", "i'm called ", "i am called "});
    if (!name.empty() && name.size() < 40) facts["user_name"] = name;

    auto loc = firstMatch({"i live in ", "i am from ", "i'm from ", "i'm in ", "i am in "});
    if (!loc.empty() && loc.size() < 50) facts["location"] = loc;

    auto job = firstMatch({"i work as ", "i work at ", "my job is "});
    if (!job.empty() && job.size() < 80) facts["job"] = job;

    auto pref = firstMatch({"i prefer ", "i like using ", "i swear by ", "i love ", "i really like "});
    if (!pref.empty() && pref.size() < 80) facts["preference"] = pref;

    auto proj = firstMatch({"i'm working on ", "i am working on ", "my project is ", "i'm building "});
    if (!proj.empty() && proj.size() < 120) facts["current_project"] = proj;

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

// Phase 8.6: Passive entity extraction — captures URLs, emails, paths, @handles, #tags
// from user utterances so the entity graph builds up across sessions.
// Cheap (regex-only, no LLM call); more nuanced extraction (people, projects) will
// layer on via LLM later when we have a reason to pay for it.
static std::vector<std::pair<std::string, std::string>> extractEntities(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;

    auto scan = [&](const std::regex& re, const char* type) {
        auto begin = std::sregex_iterator(text.begin(), text.end(), re);
        auto end   = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string m = it->str();
            if (m.size() > 1 && m.size() < 200) out.emplace_back(m, type);
        }
    };

    static const std::regex urlRe(R"(https?://[^\s]+)");
    static const std::regex emailRe(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
    static const std::regex pathRe(R"(/[A-Za-z0-9_][A-Za-z0-9_./-]{3,})");       // absolute paths
    static const std::regex handleRe(R"(@[A-Za-z0-9_]{2,30}\b)");
    static const std::regex tagRe(R"(#[A-Za-z][A-Za-z0-9_-]{1,40}\b)");

    scan(urlRe,    "url");
    scan(emailRe,  "email");
    scan(pathRe,   "path");
    scan(handleRe, "handle");
    scan(tagRe,    "tag");
    return out;
}

static std::string detectTone(const std::string& text) {
    auto t = lower(text);
    static const std::regex frustRe(
        R"((damn|dammit|shit|crap|wtf|ugh|come on|why isn'?t|not working|broken|still not|again\?|seriously|whatever))",
        std::regex::icase);
    static const std::regex urgRe(
        R"(\b(now|asap|quickly|quick|fast|hurry|right now|immediately)\b)",
        std::regex::icase);
    static const std::regex curRe(
        R"(\b(how does|how do|what is|why does|why is|explain|tell me about|curious|wonder(?:ing)?)\b)",
        std::regex::icase);
    if (std::regex_search(t, frustRe)) return "frustrated";
    if (std::regex_search(t, urgRe))   return "urgent";
    if (std::regex_search(t, curRe))   return "curious";
    return "";
}

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

static std::string currentDateLabel() {
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%A, %B %e", &lt) == 0) return "";
    return buf;
}

static std::string arg(const nlohmann::json& a, const std::string& key) {
    if (a.contains(key) && a[key].is_string()) return a[key].get<std::string>();
    if (a.contains("param") && a["param"].is_string()) return a["param"].get<std::string>();
    return "";
}

// ─── Action dispatch ───

std::string Daemon::executeAction(const AgentAction& act) {
    // Timer actions are owned by the daemon's TimerManager.
    if (act.type == "timer_set") {
        int secs = 0;
        if (act.args.contains("duration_seconds") && act.args["duration_seconds"].is_number())
            secs = act.args["duration_seconds"].get<int>();
        std::string label = arg(act.args, "label");
        if (secs <= 0) return "Invalid duration.";
        int id = timers_->addTimer(secs, label);
        return "Timer #" + std::to_string(id) + " set for " + std::to_string(secs) + " seconds.";
    }
    if (act.type == "timer_list")   return timers_->list();
    if (act.type == "timer_cancel") {
        int id = 0;
        if (act.args.contains("id") && act.args["id"].is_number())
            id = act.args["id"].get<int>();
        return timers_->cancel(id) ? "Timer cancelled." : "Timer not found.";
    }

    // Memory actions — owned by the daemon.
    if (act.type == "recall_memory") {
        std::string query = arg(act.args, "query");
        if (query.empty()) return "No query specified.";
        auto results = memory_->hybridSearch(query, 5);
        if (results.empty()) return "Nothing found in memory about that.";
        std::ostringstream out;
        for (auto& r : results) {
            out << "[" << r.entry.timestamp << " " << r.source;
            if (r.vecSim > 0) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), " sim=%.2f", r.vecSim);
                out << buf;
            }
            out << "] " << r.entry.role << ": " << r.entry.content << "\n";
        }
        return out.str();
    }
    if (act.type == "remember") {
        std::string k = arg(act.args, "key");
        std::string v = arg(act.args, "value");
        if (k.empty() || v.empty()) return "Nothing to remember.";
        memory_->setFact(k, v);
        Logger::info("Memory: LLM-stored fact " + k + " = " + v);
        return "Got it — remembered " + k + ".";
    }

    // ask_user — just speak the question; next utterance continues via LLM history.
    if (act.type == "ask") {
        std::string q = arg(act.args, "question");
        return q.empty() ? "What should I do?" : q;
    }

    return executor_->execute(act);
}

// Summarize older turns into a compact blurb so LLM context never explodes.
// Detaches to avoid stalling the user; runs every ~20 fresh turns.
void Daemon::maybeSummarize() {
    int lastId   = memory_->lastConversationId();
    int lastSumm = memory_->lastSummarizedId();
    if (lastId - lastSumm < 20) return;

    LLM*    llmPtr = llm_;
    Memory* memPtr = memory_;
    std::thread([memPtr, llmPtr, lastSumm, lastId]() {
        auto turns = memPtr->getRange(lastSumm, lastId);
        if (turns.size() < 10) return;
        std::ostringstream text;
        for (auto& t : turns)
            text << t.role << ": " << t.content << "\n";
        auto summary = llmPtr->summarize(text.str());
        if (!summary.empty()) {
            memPtr->saveSummary(summary, lastSumm, lastId);
            Logger::info("Memory: summarized turns " +
                         std::to_string(lastSumm + 1) + "-" + std::to_string(lastId));
        }
    }).detach();
}

// ─── Tool-call / ReAct handling ───
//
// v3 changes vs v2:
//   - No more brittle isMultiStep heuristic — the LLM decides via tool calls.
//     run_command still routes through ReAct so we can observe output.
//   - Wall-clock deadline (30s total) on top of step cap, so a stuck LLM
//     can't loop for 5×timeout seconds.
//   - If we hit the step cap or deadline without task_done, synthesize a
//     summary from the last observation instead of leaving the user silent.
void Daemon::handleLLMResponse(LLMResponse& response, LLMContext& ctx,
                                const std::string& origText) {
    (void)origText; // reserved for future use
    if (!response.hasAction()) {
        // Pure conversation — stream already delivered; just close it out.
        tts_->endStream();
        if (!response.speech.empty())
            memory_->save("assistant", response.speech);
        return;
    }

    // Tool calls arrived — end the text stream (may have been empty).
    tts_->endStream();

    // ReAct is only needed for actions that produce an observation worth
    // feeding back to the LLM. `run_command` is the canonical case; vision
    // tools likewise return data the LLM usually wants to summarize.
    bool needsReact = false;
    for (auto& a : response.actions) {
        if (a.type == "run" ||
            a.type == "see_screen" ||
            a.type == "describe_region" ||
            a.type == "read_screen_text") {
            needsReact = true;
            break;
        }
    }

    if (needsReact) {
        const int  maxSteps = Config::get().max_react_steps;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        int step = 0;
        std::string lastObservation;

        while (response.hasAction() && !response.done && step < maxSteps) {
            if (std::chrono::steady_clock::now() > deadline) {
                Logger::warn("ReAct: 30s wall-clock timeout");
                break;
            }

            std::string observation;
            for (auto& act : response.actions) {
                if (act.type == "done") {
                    response.done = true;
                    std::string summary = arg(act.args, "summary");
                    Logger::info("ReAct: task complete — " + summary);
                    memory_->save("assistant", "task_done:" + summary);
                    tts_->speak(summary);
                    break;
                }

                Logger::info("ReAct step " + std::to_string(step + 1) +
                             ": " + act.type + " → " + act.args.dump());

                std::string result;
                if (act.type == "run")
                    result = executor_->safeShellCapture(arg(act.args, "command"));
                else
                    result = executeAction(act);
                memory_->save("assistant", act.type + ":" + act.args.dump());

                if (!observation.empty()) observation += "\n";
                observation += result;
            }

            if (response.done) break;

            lastObservation = observation;

            if (!response.speech.empty())
                tts_->speak(response.speech);

            step++;
            if (step >= maxSteps || std::chrono::steady_clock::now() > deadline) {
                Logger::warn("ReAct: hit limit at step " + std::to_string(step) +
                             " (max=" + std::to_string(maxSteps) + ")");
                // Synthesize something from the last observation so the user
                // isn't left in silence when the LLM forgets to call task_done.
                std::string summary = lastObservation;
                if (summary.size() > 200) summary = summary.substr(0, 200) + "...";
                if (summary.empty())
                    tts_->speak("Done after " + std::to_string(step) + " steps.");
                else
                    tts_->speak(summary);
                break;
            }

            auto freshCtx = Context::capture();
            ctx.activeApp     = freshCtx.activeApp;
            ctx.activeWindow  = freshCtx.activeWindow;
            ctx.clipboard     = freshCtx.clipboard;
            ctx.screenText    = freshCtx.screenText;
            ctx.notifications = freshCtx.recentNotifications;

            tts_->startStream();
            response = llm_->reactStreaming(observation, ctx,
                [this](const std::string& delta) { tts_->feedChunk(delta); });
            tts_->endStream();
        }
    } else {
        // Simple action(s) — execute directly, no observation loop needed.
        for (auto& act : response.actions) {
            Logger::info("LLM action: " + act.type + " → " + act.args.dump());
            std::string feedback = executeAction(act);
            memory_->save("assistant", act.type + ":" + act.args.dump());
            if (response.speech.empty() && !feedback.empty())
                tts_->speak(feedback);
        }
    }

    // Speak any speech that arrived alongside tool calls.
    if (!response.speech.empty() && response.hasAction()) {
        memory_->save("assistant", response.speech);
        tts_->speak(response.speech);
    }
}

// ─── Main utterance processor ───

void Daemon::processUtterance(const std::string& rawText) {
    std::string text = rawText;

    if (isInterruptCommand(text)) {
        tts_->interrupt();
        Logger::info("ARIA: interrupted.");
        return;
    }

    // Wake-word gate — optional, controlled by ARIA_WAKE_WORD env var.
    if (wakeRequired_) {
        auto now = std::chrono::steady_clock::now();
        bool awake = (awakeUntil_ > now);
        auto stripped = stripWakeWord(text);
        if (stripped.has_value()) {
            text = *stripped;
            awakeUntil_ = now + kFollowUpWindow;
            if (text.empty()) {
                recorder_->mute();
                tts_->speak("Yes?");
                recorder_->unmute();
                Logger::info("Wake: acknowledged, awaiting follow-up.");
                return;
            }
            Logger::info("Wake: activated → " + text);
        } else if (awake) {
            awakeUntil_ = now + kFollowUpWindow;
            Logger::info("Wake: follow-up accepted.");
        } else {
            Logger::info("Wake: asleep, ignored: " + text);
            return;
        }
    }

    // Passive fact extraction — learn name, location, job, etc.
    auto newFacts = extractFacts(text);
    for (auto& [k, v] : newFacts) {
        memory_->setFact(k, v);
        Logger::info("Memory: learned " + k + " = " + v);
        lastFactKey_ = k;
    }

    // Passive entity extraction — URLs, paths, handles, tags. Silent, best-effort.
    for (auto& [name, type] : extractEntities(text)) {
        memory_->upsertEntity(name, type);
    }

    // Correction path: "actually X" / "I meant X" rewrites the most recent fact.
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
                    memory_->setFact(lastFactKey_, v);
                    Logger::info("Memory: corrected " + lastFactKey_ + " → " + v);
                }
            }
        }
    }
    if (!newFacts.empty())
        lastFactTime_ = std::chrono::steady_clock::now();

    // Intent classifier runs before the word-count filter so short known
    // commands ("play", "mute") still dispatch.
    AgentAction directAction = classifyIntent(text);

    if (directAction.empty() && wordCount(text) <= 2) {
        Logger::info("Skipped short utterance: " + text);
        return;
    }

    recorder_->mute();

    if (!directAction.empty()) {
        Logger::info("Intent: " + directAction.type + " → " + directAction.args.dump());
        std::string feedback = executeAction(directAction);
        memory_->save("user", text);
        memory_->save("assistant", directAction.type + ":" + directAction.args.dump());
        if (!feedback.empty()) tts_->speak(feedback);
        recorder_->unmute();
        maybeSummarize();
        return;
    }

    // Unknown command → LLM path.
    auto sysCtx = Context::capture();
    Logger::info("Context: app=" + sysCtx.activeApp + " window=" + sysCtx.activeWindow);

    // Fast-path direct context queries — avoid the LLM round-trip.
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
        memory_->save("user", text);
        memory_->save("assistant", directAnswer);
        tts_->speak(directAnswer);
        recorder_->unmute();
        maybeSummarize();
        return;
    }

    // Check Ollama health before wasting effort building context.
    if (!llm_->isAvailable()) {
        Logger::warn("LLM: Ollama unreachable, retrying once...");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!llm_->isAvailable()) {
            Logger::error("LLM: still unreachable, giving up on this utterance");
            memory_->save("user", text);
            tts_->speak("I'm offline right now, try again in a moment.");
            recorder_->unmute();
            return;
        }
    }

    LLMContext ctx;
    ctx.activeApp     = sysCtx.activeApp;
    ctx.activeWindow  = sysCtx.activeWindow;
    ctx.clipboard     = sysCtx.clipboard;
    ctx.screenText    = sysCtx.screenText;
    ctx.notifications = sysCtx.recentNotifications;
    ctx.memorySummary = memory_->getSummary();
    ctx.tone          = detectTone(text);
    ctx.timeOfDay     = currentTimeOfDay();
    ctx.dateLabel     = currentDateLabel();
    if (!ctx.tone.empty())
        Logger::info("Tone: " + ctx.tone);

    auto userName = memory_->getFact("user_name");
    std::string prompt = text;
    if (!userName.empty() &&
        (lower(text).find("hello") != std::string::npos ||
         lower(text).find("hey")   != std::string::npos))
        prompt = text + " (user's name is " + userName + ")";

    // Streaming LLM → Streaming TTS
    tts_->startStream();
    auto response = llm_->thinkStreaming(prompt, ctx,
        [this](const std::string& delta) { tts_->feedChunk(delta); });

    memory_->save("user", text);

    handleLLMResponse(response, ctx, text);

    recorder_->unmute();
    maybeSummarize();
}

// ─── Run loop ───

void Daemon::run() {
    Logger::info("Initializing pipeline...");
    const auto& cfg = Config::get();

    // Phase 10: sweep any orphan /tmp/aria_vlm_*.png left behind by a prior
    // crashed run before we start generating new ones this session.
    Vision::cleanupStaleTempFiles();

    Transcriber transcriber(cfg.whisper_model);
    Memory      memory(cfg.db_path);
    LLM         llm(cfg.ollama_model, &memory);
    Executor    executor;
    TTS         tts(cfg.piper_model);

    // Publish member pointers for the extracted methods.
    transcriber_ = &transcriber;
    memory_      = &memory;
    llm_         = &llm;
    executor_    = &executor;
    tts_         = &tts;

    // Startup Ollama probe — warn but don't bail; intent-only commands
    // still work without the LLM.
    if (!llm.isAvailable())
        Logger::warn("LLM: Ollama unreachable at " + cfg.ollama_url +
                     " — LLM queries will fail until it's up.");

    std::mutex              qMutex;
    std::queue<std::string> speechQueue;
    std::condition_variable qCV;

    Recorder recorder("/tmp/aria_speech.wav",
        [&](const std::string& wavPath) {
            std::lock_guard<std::mutex> lock(qMutex);
            speechQueue.push(wavPath);
            qCV.notify_one();
        });
    recorder_ = &recorder;

    TimerManager timers([&tts, &recorder](const std::string& label) {
        recorder.mute();
        tts.speak("Timer done: " + label);
        system(("notify-send 'ARIA Timer' " + std::string("'") + label + "'").c_str());
        recorder.unmute();
    });
    timers_ = &timers;

    // ── Phase 9: audio wake word ──
    // Default: audio wake word enabled. Falls back to ALWAYS_ON recorder if
    // the model files are missing or ORT init fails.
    std::shared_ptr<WakeWord> ww;
    if (cfg.wake_enabled && !cfg.always_listen) {
        ww = std::make_shared<WakeWord>();
        WakeWord::Config wcfg;
        wcfg.melspecModelPath   = cfg.wake_model_dir + "/melspectrogram.onnx";
        wcfg.embeddingModelPath = cfg.wake_model_dir + "/embedding_model.onnx";
        wcfg.wakewordModelPath  = cfg.wake_model_dir + "/" + cfg.wake_model;
        wcfg.threshold          = cfg.wake_threshold;
        wcfg.triggerLevel       = cfg.wake_trigger;
        wcfg.refractoryFrames   = cfg.wake_refractory;
        wcfg.debug              = cfg.wake_debug;
        if (!ww->init(wcfg)) {
            Logger::warn("Wake-word: init failed — falling back to always-on mode.");
            ww.reset();
        }
    }

    // Callback: play the wake cue. Spawned as a detached thread so the
    // recorder's capture loop keeps draining the audio pipe — otherwise TTS
    // playback queues in the PulseAudio buffer and gets re-fed into the wake
    // detector after unmute.
    auto wakeCue = [&tts, &recorder, &cfg]() {
        if (cfg.wake_cue == "none") return;
        std::thread([&tts, &recorder, &cfg]() {
            recorder.mute();
            if (cfg.wake_cue == "tone") {
                system("paplay --volume=30000 /usr/share/sounds/freedesktop/stereo/message.oga >/dev/null 2>&1 &");
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            } else {
                tts.speak("Yes?");
            }
            recorder.unmute();
        }).detach();
    };

    if (ww) {
        recorder.setWakeWord(Recorder::Mode::WAKE_WORD, ww,
                             std::chrono::seconds(cfg.wake_window_sec),
                             wakeCue);
        Logger::info("Wake-word: audio gate active (model=" + cfg.wake_model +
                     ", threshold=" + std::to_string(cfg.wake_threshold) + ").");
    } else {
        Logger::info("Wake-word: disabled (always-on listening).");
    }

    // Phase 10: non-blocking VLM prewarm. Pushes moondream into Ollama's
    // resident set in the background so the first real see_screen call
    // doesn't eat the 3–5 s model-swap penalty.
    Vision::prewarm();

    // Text-prefix wake (legacy) stays useful when audio wake is off and the
    // user still wants a command gate. With audio wake active, the recorder
    // already gates so text check is a no-op.
    wakeRequired_ = false;
    if (const char* v = std::getenv("ARIA_WAKE_TEXT"); v && v[0] == '1')
        wakeRequired_ = true;

    std::thread processor([&]() {
        while (!shutdownRequested.load()) {
            std::unique_lock<std::mutex> lock(qMutex);
            qCV.wait_for(lock, std::chrono::milliseconds(100),
                [&]{ return !speechQueue.empty() || shutdownRequested.load(); });
            if (speechQueue.empty()) continue;
            std::string wavPath = speechQueue.front();
            speechQueue.pop();
            lock.unlock();

            if (!ariaActive_.load()) continue;

            std::string text = transcriber.transcribe(wavPath);

            // After 3 consecutive whisper failures, tell the user something's
            // wrong — before that we stay silent to avoid spamming on misreads.
            if (text.empty() && transcriber.consecutiveFailures() == 3) {
                Logger::error("Transcriber: 3 consecutive failures, announcing.");
                recorder.mute();
                tts.speak("Having trouble hearing. Check your microphone.");
                recorder.unmute();
                transcriber.resetFailures();
                continue;
            }

            if (text.empty() || text.find("[BLANK_AUDIO]") != std::string::npos) {
                Logger::info("VAD: blank, skipping.");
                continue;
            }
            Logger::info("You said: " + text);

            processUtterance(text);
        }
    });

    recorder.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // Startup greeting with name if known.
    recorder.mute();
    auto userName = memory.getFact("user_name");
    tts.speak(userName.empty() ? "Online." : "Online, " + userName + ".");
    recorder.unmute();

    Logger::info("ARIA ready.");

    while (!shutdownRequested.load()) {
        if (pauseToggle.exchange(false)) {
            bool nowActive = !ariaActive_.load();
            ariaActive_.store(nowActive);
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
