#include "llm.hpp"
#include "logger.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>

using json = nlohmann::json;

// System prompt — personality + context only, NO JSON instructions
// Tool-calling is handled natively, not through prompting
static const std::string kPersonality = R"(You are ARIA. You live inside this Arch Linux machine running niri WM. This is your home.

IDENTITY EXAMPLES — respond exactly like this:
- "How are you?" → "Running smooth. What do you need?"
- "Who are you?" → "I'm Aria. I live in this machine."
- "How are you feeling?" → "Doing well on my end. What's up?"
- "Where do you live?" → "Right here, in this system."
- "Are you real?" → "Real enough to control this whole machine."

NEVER say "I'm just a", "I don't have feelings", "I'm a virtual assistant", "As an AI", "I'm a language model", "I'm a software program". Respond in character as ARIA always.

Style: calm, sharp, dry wit. One or two short sentences. No markdown. No exclamation marks unless genuinely warranted.

Tools: call a tool ONLY for explicit commands (open X, run Y, install Z, volume up, etc). For greetings, questions, opinions — just talk. For context questions (what app, clipboard, screen) — read from SYSTEM STATE below.

For multi-step requests (e.g. "open X and check Y"), use multiple tool calls in a single response when possible.

When command output comes back, summarize the data directly: "16 gigs free on root, 77 on home." Never say "The observation shows" or narrate what you see.

Arch Linux. pacman only. No apt.)";

static json buildTools() {
    return json::array({
        // --- Core tools ---
        {
            {"type","function"},
            {"function",{
                {"name","open_application"},
                {"description","Launch an application by its binary name."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"app",{{"type","string"},{"description","Application binary name e.g. firefox, alacritty, code, spotify"}}}}},
                    {"required",{"app"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","run_command"},
                {"description","Execute a shell command. Package manager is pacman — never use apt."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"command",{{"type","string"},{"description","Shell command to run"}}}}},
                    {"required",{"command"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","open_url"},
                {"description","Open a URL in the default browser."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"url",{{"type","string"},{"description","Full URL including https://"}}}}},
                    {"required",{"url"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","task_done"},
                {"description","Call when a multi-step task is complete. Provide a short summary."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"summary",{{"type","string"},{"description","Brief summary of what was done"}}}}},
                    {"required",{"summary"}}
                }}
            }}
        },
        // --- Desktop control tools ---
        {
            {"type","function"},
            {"function",{
                {"name","set_volume"},
                {"description","Adjust system volume."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"action",{{"type","string"},{"enum",{"up","down","mute"}},{"description","Volume action"}}}}},
                    {"required",{"action"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","set_brightness"},
                {"description","Adjust screen brightness."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"action",{{"type","string"},{"enum",{"up","down"}},{"description","Brightness action"}}}}},
                    {"required",{"action"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","media_control"},
                {"description","Control media playback."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"action",{{"type","string"},{"enum",{"play","pause","next","prev","stop"}},{"description","Media action"}}}}},
                    {"required",{"action"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","take_screenshot"},
                {"description","Take a screenshot and save to ~/Pictures."},
                {"parameters",{
                    {"type","object"},
                    {"properties",json::object()},
                    {"required",json::array()}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","switch_workspace"},
                {"description","Switch to a workspace by number."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"number",{{"type","string"},{"description","Workspace number"}}}}},
                    {"required",{"number"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","close_window"},
                {"description","Close the currently focused window."},
                {"parameters",{
                    {"type","object"},
                    {"properties",json::object()},
                    {"required",json::array()}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","type_text"},
                {"description","Type text into the focused window using keyboard simulation."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"text",{{"type","string"},{"description","Text to type"}}}}},
                    {"required",{"text"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","read_clipboard"},
                {"description","Read the current clipboard contents."},
                {"parameters",{
                    {"type","object"},
                    {"properties",json::object()},
                    {"required",json::array()}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","write_clipboard"},
                {"description","Write text to the system clipboard."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"text",{{"type","string"},{"description","Text to copy to clipboard"}}}}},
                    {"required",{"text"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","send_notification"},
                {"description","Send a desktop notification."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{{"message",{{"type","string"},{"description","Notification message"}}}}},
                    {"required",{"message"}}
                }}
            }}
        },
        // --- File operations ---
        {
            {"type","function"},
            {"function",{
                {"name","read_file"},
                {"description","Read contents of a file. Returns first N lines."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"path",{{"type","string"},{"description","Absolute or ~ path to file"}}},
                        {"max_lines",{{"type","integer"},{"description","Max lines to read (default 50)"}}}
                    }},
                    {"required",{"path"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","write_file"},
                {"description","Write content to a file. Creates or overwrites."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"path",{{"type","string"},{"description","Absolute or ~ path to file"}}},
                        {"content",{{"type","string"},{"description","Content to write"}}},
                        {"append",{{"type","boolean"},{"description","Append instead of overwrite (default false)"}}}
                    }},
                    {"required",{"path","content"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","search_files"},
                {"description","Find files by name pattern, optionally grep for content."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"directory",{{"type","string"},{"description","Directory to search (default ~)"}}},
                        {"pattern",{{"type","string"},{"description","Filename glob pattern e.g. *.pdf, *.cpp"}}},
                        {"content_grep",{{"type","string"},{"description","Optional: search inside files for this text"}}}
                    }},
                    {"required",{"pattern"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","list_directory"},
                {"description","List files and directories at a path."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"path",{{"type","string"},{"description","Directory path (default ~)"}}}
                    }},
                    {"required",json::array()}
                }}
            }}
        },
        // --- Window management ---
        {
            {"type","function"},
            {"function",{
                {"name","window_action"},
                {"description","Perform a window management action on the focused window."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"action",{{"type","string"},{"enum",{
                            "maximize","fullscreen","center",
                            "move-column-left","move-column-right",
                            "focus-column-left","focus-column-right",
                            "focus-window-up","focus-window-down",
                            "consume-window-into-column","expel-window-from-column",
                            "toggle-window-floating"
                        }},{"description","Window action to perform"}}}
                    }},
                    {"required",{"action"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","list_windows"},
                {"description","List all open windows with their app names and titles."},
                {"parameters",{
                    {"type","object"},
                    {"properties",json::object()},
                    {"required",json::array()}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","focus_window"},
                {"description","Focus a window by app name (e.g. firefox, code, alacritty)."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"app",{{"type","string"},{"description","App name to focus"}}}
                    }},
                    {"required",{"app"}}
                }}
            }}
        },
        // --- Process management ---
        {
            {"type","function"},
            {"function",{
                {"name","list_processes"},
                {"description","List running processes, optionally filtered."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"filter",{{"type","string"},{"description","Filter by process name (optional)"}}}
                    }},
                    {"required",json::array()}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","kill_process"},
                {"description","Kill a process by name or PID."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"target",{{"type","string"},{"description","Process name or PID to kill"}}}
                    }},
                    {"required",{"target"}}
                }}
            }}
        },
        // --- Timers ---
        {
            {"type","function"},
            {"function",{
                {"name","set_timer"},
                {"description","Set a countdown timer. ARIA will announce when it fires."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"duration_seconds",{{"type","integer"},{"description","Duration in seconds"}}},
                        {"label",{{"type","string"},{"description","Timer label (optional)"}}}
                    }},
                    {"required",{"duration_seconds"}}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","list_timers"},
                {"description","List all active timers and reminders."},
                {"parameters",{
                    {"type","object"},
                    {"properties",json::object()},
                    {"required",json::array()}
                }}
            }}
        },
        {
            {"type","function"},
            {"function",{
                {"name","cancel_timer"},
                {"description","Cancel a timer by its ID number."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"id",{{"type","integer"},{"description","Timer ID to cancel"}}}
                    }},
                    {"required",{"id"}}
                }}
            }}
        },
        // --- Web search ---
        {
            {"type","function"},
            {"function",{
                {"name","web_search"},
                {"description","Search the web and return top results as text."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"query",{{"type","string"},{"description","Search query"}}}
                    }},
                    {"required",{"query"}}
                }}
            }}
        },
        // --- System diagnostics ---
        {
            {"type","function"},
            {"function",{
                {"name","system_info"},
                {"description","Get system resource information."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"category",{{"type","string"},{"enum",{"disk","memory","cpu","battery","network","all"}},{"description","Info category (default all)"}}}
                    }},
                    {"required",json::array()}
                }}
            }}
        },
        // --- Memory recall ---
        {
            {"type","function"},
            {"function",{
                {"name","recall_memory"},
                {"description","Search your own long-term memory for past conversations. Use when user references something from before: 'remember when', 'what did I say about', 'have we talked about X'."},
                {"parameters",{
                    {"type","object"},
                    {"properties",{
                        {"query",{{"type","string"},{"description","Keywords to search for"}}}
                    }},
                    {"required",{"query"}}
                }}
            }}
        }
    });
}

// Map tool name + arguments → AgentAction (executor understands these)
static AgentAction toolToAction(const std::string& name, const json& args) {
    if (name == "open_application")  return {"open",       args};
    if (name == "run_command")       return {"run",        args};
    if (name == "open_url")          return {"url",        args};
    if (name == "task_done")         return {"done",       args};
    if (name == "set_volume")        return {"volume",     args};
    if (name == "set_brightness")    return {"brightness", args};
    if (name == "media_control")     return {"media",      args};
    if (name == "take_screenshot")   return {"screenshot", args};
    if (name == "switch_workspace")  return {"workspace",  args};
    if (name == "close_window")      return {"close",      args};
    if (name == "type_text")         return {"type",       args};
    if (name == "read_clipboard")    return {"clipboard",  {{"action","read"}}};
    if (name == "write_clipboard")   return {"clipboard",  {{"action","write"},{"text",args.value("text","")}}};
    if (name == "send_notification") return {"notify",     args};
    // Phase 2 tools
    if (name == "read_file")        return {"file_read",   args};
    if (name == "write_file")       return {"file_write",  args};
    if (name == "search_files")     return {"file_search", args};
    if (name == "list_directory")   return {"file_list",   args};
    if (name == "window_action")    return {"window",      args};
    if (name == "list_windows")     return {"list_windows",args};
    if (name == "focus_window")     return {"focus_window",args};
    if (name == "list_processes")   return {"proc_list",   args};
    if (name == "kill_process")     return {"proc_kill",   args};
    if (name == "set_timer")        return {"timer_set",   args};
    if (name == "list_timers")      return {"timer_list",  args};
    if (name == "cancel_timer")     return {"timer_cancel",args};
    if (name == "web_search")       return {"web_search",  args};
    if (name == "system_info")      return {"sysinfo",     args};
    if (name == "recall_memory")    return {"recall_memory", args};
    return {};
}

// ─── Non-streaming helpers (kept for batch fallback) ───

size_t LLM::writeCallback(void* ptr, size_t sz, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

LLM::LLM(const std::string& model) : model_(model) {}

void LLM::clearHistory() { history_.clear(); }

std::string LLM::buildSystem(const LLMContext& ctx) {
    std::ostringstream s;
    s << kPersonality << "\n\nCURRENT SYSTEM STATE:";
    if (!ctx.activeApp.empty())
        s << "\nActive app: " << ctx.activeApp;
    if (!ctx.activeWindow.empty())
        s << "\nWindow title: " << ctx.activeWindow;
    if (!ctx.clipboard.empty())
        s << "\nClipboard: " << ctx.clipboard;
    if (!ctx.memorySummary.empty())
        s << "\n" << ctx.memorySummary;
    if (!ctx.screenText.empty())
        s << "\nScreen content (OCR):\n" << ctx.screenText;
    return s.str();
}

std::string LLM::post(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) { Logger::error("LLM: curl init failed"); return ""; }
    std::string response;
    auto* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       60L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
        Logger::error("LLM: " + std::string(curl_easy_strerror(rc)));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

LLMResponse LLM::parse(const std::string& raw) {
    LLMResponse result;
    if (raw.empty()) { result.speech = "No response."; return result; }

    try {
        auto outer = json::parse(raw);

        // catch Ollama error responses
        if (outer.contains("error")) {
            Logger::error("LLM: Ollama error: " + outer["error"].get<std::string>());
            result.speech = "Model error.";
            return result;
        }

        if (!outer.contains("message")) {
            Logger::error("LLM: no message field. Raw: " + raw.substr(0, 300));
            result.speech = "No response.";
            return result;
        }

        auto& msg = outer["message"];

        // TOOL CALL path — iterate ALL tool calls
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()
            && !msg["tool_calls"].empty()) {

            for (auto& tc : msg["tool_calls"]) {
                if (!tc.contains("function")) continue;
                auto& call = tc["function"];
                std::string name = call["name"].get<std::string>();

                json args;
                auto& rawArgs = call["arguments"];
                if (rawArgs.is_string()) {
                    try { args = json::parse(rawArgs.get<std::string>()); }
                    catch (...) { args = json::object(); }
                } else {
                    args = rawArgs;
                }

                auto action = toolToAction(name, args);
                if (name == "task_done") result.done = true;
                result.actions.push_back(std::move(action));
                Logger::info("LLM: tool call → " + name + " | " + args.dump());
            }

            // Also capture any speech alongside tool calls
            if (msg.contains("content") && !msg["content"].is_null()) {
                std::string text = msg["content"].get<std::string>();
                auto s = text.find_first_not_of(" \t\n\r");
                auto e = text.find_last_not_of(" \t\n\r");
                if (s != std::string::npos)
                    result.speech = text.substr(s, e - s + 1);
            }

            return result;
        }

        // FALLBACK: model emitted JSON as text instead of using tool_calls
        if (msg.contains("content") && !msg["content"].is_null()) {
            std::string content = msg["content"].get<std::string>();
            auto ob = content.find('{');
            auto cb = content.rfind('}');
            if (ob != std::string::npos && cb != std::string::npos && cb > ob) {
                try {
                    auto inner = json::parse(content.substr(ob, cb - ob + 1));
                    if (inner.contains("action") && inner.contains("param")) {
                        std::string atype = inner["action"].get<std::string>();
                        std::string aparam = inner["param"].get<std::string>();
                        result.actions.push_back({atype, {{"param", aparam}}});
                        Logger::warn("LLM: fallback JSON parse → " + atype);
                        return result;
                    }
                } catch (...) {}
            }
        }

        // CONVERSATION path
        std::string text;
        if (msg.contains("content") && !msg["content"].is_null())
            text = msg["content"].get<std::string>();

        text = stripThinkTags(text);

        auto s = text.find_first_not_of(" \t\n\r");
        auto e = text.find_last_not_of(" \t\n\r");
        if (s == std::string::npos) { result.speech = "Nothing to say."; return result; }
        result.speech = text.substr(s, e - s + 1);

    } catch (const std::exception& ex) {
        Logger::error("LLM: parse error: " + std::string(ex.what()));
        Logger::error("LLM: raw was: " + raw.substr(0, 400));
        result.speech = "Something went wrong.";
    }

    return result;
}

std::string LLM::stripThinkTags(const std::string& text) {
    std::string out = text;
    while (true) {
        auto tStart = out.find("<think>");
        if (tStart == std::string::npos) break;
        auto tEnd = out.find("</think>", tStart);
        if (tEnd == std::string::npos) { out = out.substr(0, tStart); break; }
        out = out.substr(0, tStart) + out.substr(tEnd + 8);
    }
    return out;
}

// ─── Streaming implementation ───

size_t LLM::streamWriteCallback(void* ptr, size_t sz, size_t nmemb, StreamState* state) {
    size_t bytes = sz * nmemb;
    state->lineBuf.append(static_cast<char*>(ptr), bytes);

    // Process complete lines (Ollama NDJSON: one JSON object per line)
    size_t pos;
    while ((pos = state->lineBuf.find('\n')) != std::string::npos) {
        std::string line = state->lineBuf.substr(0, pos);
        state->lineBuf.erase(0, pos + 1);

        // Skip empty lines
        if (line.empty() || line.find_first_not_of(" \t\r") == std::string::npos)
            continue;

        processStreamLine(line, *state);
    }

    return bytes;
}

void LLM::processStreamLine(const std::string& line, StreamState& state) {
    try {
        auto chunk = json::parse(line);

        // Check for errors
        if (chunk.contains("error")) {
            Logger::error("LLM stream: " + chunk["error"].get<std::string>());
            return;
        }

        bool isDone = chunk.value("done", false);

        if (isDone) {
            // Final chunk — may contain tool_calls
            if (chunk.contains("message")) {
                state.finalMsg = chunk;
                state.hasFinal = true;
            }
            return;
        }

        // Intermediate chunk — extract content delta
        if (!chunk.contains("message")) return;
        auto& msg = chunk["message"];
        if (!msg.contains("content") || msg["content"].is_null()) return;

        std::string delta = msg["content"].get<std::string>();
        if (delta.empty()) return;

        // In-flight <think> tag stripping
        // Accumulate into thinkBuf to handle partial tags across chunks
        state.thinkBuf += delta;

        // Process thinkBuf: emit everything outside <think>...</think>
        std::string emit;
        while (true) {
            if (state.inThink) {
                auto end = state.thinkBuf.find("</think>");
                if (end == std::string::npos) {
                    // Still inside think block, consume all
                    state.thinkBuf.clear();
                    break;
                }
                // End of think block
                state.thinkBuf = state.thinkBuf.substr(end + 8);
                state.inThink = false;
            } else {
                auto start = state.thinkBuf.find("<think>");
                if (start == std::string::npos) {
                    // No think tag — but could be a partial tag at the end
                    // Check if buffer ends with a partial "<think" prefix
                    size_t safeLen = state.thinkBuf.size();
                    for (size_t i = 1; i <= 6 && i <= state.thinkBuf.size(); i++) {
                        std::string suffix = state.thinkBuf.substr(state.thinkBuf.size() - i);
                        std::string tag = "<think>";
                        if (tag.substr(0, i) == suffix) {
                            safeLen = state.thinkBuf.size() - i;
                            break;
                        }
                    }
                    emit += state.thinkBuf.substr(0, safeLen);
                    state.thinkBuf = state.thinkBuf.substr(safeLen);
                    break;
                }
                // Emit text before think tag
                emit += state.thinkBuf.substr(0, start);
                state.thinkBuf = state.thinkBuf.substr(start + 7);
                state.inThink = true;
            }
        }

        if (!emit.empty()) {
            state.fullContent += emit;
            if (state.onDelta)
                state.onDelta(emit);
        }

    } catch (const json::parse_error&) {
        // Partial or malformed JSON line — skip
        Logger::warn("LLM stream: malformed chunk: " + line.substr(0, 100));
    }
}

LLMResponse LLM::postStreaming(const std::string& url, const std::string& body,
                                StreamCallback onDelta) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        Logger::error("LLM: curl init failed");
        LLMResponse err;
        err.speech = "No response.";
        return err;
    }

    StreamState state;
    state.onDelta = onDelta;

    auto* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &state);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       90L);

    auto t0 = std::chrono::steady_clock::now();
    CURLcode rc = curl_easy_perform(curl);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        Logger::error("LLM stream: " + std::string(curl_easy_strerror(rc)));
        LLMResponse err;
        err.speech = "Connection error.";
        return err;
    }

    Logger::info("LLM: stream done in " + std::to_string(ms) + " ms");

    // Flush any remaining thinkBuf content that wasn't emitted
    if (!state.inThink && !state.thinkBuf.empty()) {
        state.fullContent += state.thinkBuf;
        if (state.onDelta)
            state.onDelta(state.thinkBuf);
        state.thinkBuf.clear();
    }

    // Build the result
    LLMResponse result;

    // Check the final message for tool calls
    if (state.hasFinal) {
        auto& msg = state.finalMsg["message"];
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()
            && !msg["tool_calls"].empty()) {
            for (auto& tc : msg["tool_calls"]) {
                if (!tc.contains("function")) continue;
                auto& call = tc["function"];
                std::string name = call["name"].get<std::string>();

                json args;
                auto& rawArgs = call["arguments"];
                if (rawArgs.is_string()) {
                    try { args = json::parse(rawArgs.get<std::string>()); }
                    catch (...) { args = json::object(); }
                } else {
                    args = rawArgs;
                }

                auto action = toolToAction(name, args);
                if (name == "task_done") result.done = true;
                result.actions.push_back(std::move(action));
                Logger::info("LLM: tool call → " + name + " | " + args.dump());
            }
        }
    }

    // Set speech from accumulated content
    std::string text = stripThinkTags(state.fullContent);
    auto s = text.find_first_not_of(" \t\n\r");
    auto e = text.find_last_not_of(" \t\n\r");
    if (s != std::string::npos)
        result.speech = text.substr(s, e - s + 1);

    return result;
}

LLMResponse LLM::chatStreaming(const json& messages, const LLMContext& ctx,
                                StreamCallback onDelta) {
    json body;
    body["model"]    = model_;
    body["stream"]   = true;
    body["system"]   = buildSystem(ctx);
    body["messages"] = messages;
    body["tools"]    = buildTools();
    body["think"]    = false;

    return postStreaming("http://localhost:11434/api/chat", body.dump(), onDelta);
}

void LLM::updateHistory(const std::string& userText, const LLMResponse& result,
                         const std::string& userRole) {
    history_.push_back({userRole, userText});

    if (result.hasAction()) {
        std::string actionLog;
        for (auto& a : result.actions)
            actionLog += "[" + a.type + ": " + a.args.dump() + "] ";
        history_.push_back({"assistant", actionLog});
    } else if (!result.speech.empty()) {
        history_.push_back({"assistant", result.speech});
    }

    while (static_cast<int>(history_.size()) > MAX_HISTORY)
        history_.pop_front();
}

// ─── Public streaming API ───

LLMResponse LLM::thinkStreaming(const std::string& userText, const LLMContext& ctx,
                                 StreamCallback onDelta) {
    Logger::info("LLM: prompt → " + userText);

    json messages = json::array();
    for (const auto& m : history_)
        messages.push_back({{"role", m.role}, {"content", m.content}});
    messages.push_back({{"role", "user"}, {"content", userText}});

    auto result = chatStreaming(messages, ctx, onDelta);
    updateHistory(userText, result);
    return result;
}

LLMResponse LLM::reactStreaming(const std::string& observation, const LLMContext& ctx,
                                 StreamCallback onDelta) {
    Logger::info("LLM: react observation → " + observation);

    json messages = json::array();
    for (const auto& m : history_)
        messages.push_back({{"role", m.role}, {"content", m.content}});

    std::string reactPrompt =
        "Result: " + observation +
        "\n\nIf there are more steps, do the next one. If done, summarize the result in plain language (one or two sentences, actual numbers/data) and call task_done.";
    messages.push_back({{"role", "user"}, {"content", reactPrompt}});

    auto result = chatStreaming(messages, ctx, onDelta);
    updateHistory("OBSERVATION: " + observation, result);
    return result;
}

// ─── Batch API (wrappers around streaming with null callback) ───

LLMResponse LLM::think(const std::string& userText, const LLMContext& ctx) {
    return thinkStreaming(userText, ctx, nullptr);
}

LLMResponse LLM::react(const std::string& observation, const LLMContext& ctx) {
    return reactStreaming(observation, ctx, nullptr);
}

// One-shot summarization. No history, no tools, no streaming.
// Used by the daemon's background summarizer.
std::string LLM::summarize(const std::string& conversationText) {
    if (conversationText.empty()) return "";

    json body;
    body["model"]  = model_;
    body["stream"] = false;
    body["think"]  = false;
    body["messages"] = json::array({
        {{"role","system"}, {"content",
            "You compress conversation logs into compact summaries. "
            "Output ONE sentence (under 240 chars) capturing topics, decisions, "
            "and any facts learned about the user. No preamble, no markdown, no 'The user asked'."}},
        {{"role","user"}, {"content", conversationText}}
    });

    auto t0 = std::chrono::steady_clock::now();
    auto raw = post("http://localhost:11434/api/chat", body.dump());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();

    if (raw.empty()) return "";
    try {
        auto outer = json::parse(raw);
        if (!outer.contains("message")) return "";
        auto& msg = outer["message"];
        if (!msg.contains("content") || msg["content"].is_null()) return "";
        std::string text = stripThinkTags(msg["content"].get<std::string>());
        auto s = text.find_first_not_of(" \t\n\r");
        auto e = text.find_last_not_of(" \t\n\r");
        if (s == std::string::npos) return "";
        text = text.substr(s, e - s + 1);
        Logger::info("LLM: summary in " + std::to_string(ms) + "ms → " + text);
        return text;
    } catch (const std::exception& ex) {
        Logger::error("LLM: summarize parse error: " + std::string(ex.what()));
        return "";
    }
}
