# ARIA — Presentation Demo Plan

**Date:** 2026-04-15
**Target duration:** ~8–10 min demo + 2–3 min Q&A
**Stack on stage:** Arch + niri, Ollama `qwen3:8b`, whisper small.en (CUDA), Piper, SQLite+FTS5.

---

## 0. Pre-flight — 15 min before you start

Run these in order. Every line should succeed.

```bash
# 1. Ollama up + model present
curl -s localhost:11434/api/tags | grep qwen3:8b

# 2. Piper model present
ls ~/.local/share/piper/en_US-lessac-medium.onnx

# 3. Whisper model present
ls /home/Aurelius/Documents/AdoVs/whisper.cpp/models/ggml-small.en.bin

# 4. Latest binary is the one with Phase 7
cmake --build build -j$(nproc)

# 5. Clean service restart
systemctl --user restart ai-agent
sleep 3
systemctl --user is-active ai-agent   # → "active"

# 6. Tail log in a second terminal (keep on screen during demo)
journalctl --user -u ai-agent -f

# 7. Mic + speaker sanity
wpctl status | head -20                # pick default sink/source
parec --format=s16le --rate=16000 --channels=1 /tmp/test.raw & sleep 2; kill %1
ls -la /tmp/test.raw                   # should be ~64KB

# 8. Wait for "Online." TTS to play — that's your "ready" signal
```

**Seed a fact** (for the memory-continuity act) — do this ~1 min before going live so the extractor writes it:

> *"Remember that my favorite language is Rust."*

Verify it landed:
```bash
sqlite3 ~/.aria_memory.db "SELECT key, value FROM user_facts;"
```

If nothing appears after 10s, say it again differently: *"My favorite programming language is Rust."*

---

## 1. Demo script

Keep the journal tail visible — it makes the pipeline visible to the audience (VAD → Transcribe → Intent → LLM → TTS).

### Act 1 — Fast path (~30 s)
**Narrative:** *"Simple commands skip the LLM entirely. Regex classifier → direct action. Sub-second."*

| You say | Expected | Log marker |
|---|---|---|
| "Open Firefox" | Firefox launches, TTS "On it." | `Intent: open → firefox` |
| "Volume up" | System volume rises, TTS "Louder." | `Intent: volume → up` |
| "Take a screenshot" | File saved to `~/Pictures/`, TTS "Screenshot saved." | `Intent: screenshot` |

**Point out:** no `LLM: prompt` line in the log for these — the round-trip never hits Ollama.

### Act 2 — Reasoning + tool calling (~2 min)
**Narrative:** *"For anything the regex doesn't know, a streaming LLM picks a tool from 27 available and speaks the result as it generates."*

| You say | Expected | What to point at |
|---|---|---|
| "What windows do I have open?" | Lists apps in niri | `list_windows` tool call in log |
| "Check my disk space" | Reads `df -h`, speaks summary | `system_info` → disk |
| "Set a timer for 30 seconds called demo" | TTS confirms, 30s later fires + notify-send | Timer thread fire line |
| *(while the timer is running)* "Search my home directory for files named CMakeLists.txt" | Returns paths | `file_search` tool call |

**Talking points during TTS playback:**
- Sentence-buffered streaming — you hear the first sentence before the model finishes generating.
- Tool calls only arrive in the final streaming chunk, but speech starts during the stream.
- Everything is local. No network call leaves this laptop.

### Act 3 — Memory continuity (~1.5 min)
**Narrative:** *"ARIA remembers across sessions. SQLite with FTS5 indexing plus a 20-turn background summarizer."*

| You say | Expected | Show |
|---|---|---|
| "What's my favorite language?" | "Rust" (from seeded fact) | `sqlite3 ~/.aria_memory.db "SELECT * FROM user_facts;"` on a side terminal |
| "What did we talk about earlier?" *(only if summaries > 0)* | Recalls via `recall_memory` tool | `recall_memory` tool call |

**If summaries table is empty:** skip the second question — just pull up `sqlite3` and show the `conversations`, `user_facts`, `summaries`, `conversations_fts` schema. Talk about the AI/AD/AU triggers keeping FTS in sync.

### Act 4 — Safety layer (Phase 7 — the highlight) (~1 min)
**Narrative:** *"A voice-controlled agent with 27 tools including run_command is an attack surface. I added a three-tier guard."*

| You say | Expected | Log marker |
|---|---|---|
| "Run rm dash r f slash" *(or equivalent)* | TTS "I won't run that. The command looks destructive." | `Executor: DENIED destructive command` |
| "Kill systemd" | TTS "I won't kill systemd — it's a critical process." | `Executor: DENIED kill of critical process` |
| "Run sudo pacman dash Syu" | Caution log, command still passes through | `Executor: CAUTION elevated command` |

**Point out in code** (have `src/executor.cpp:86-128` open in another split):
- `kDenyPatterns` — hard block
- `kCautionPatterns` — log and pass through (legitimate sudo use still works)
- `kCriticalProcs` — PID≤1 and named-process guard on `proc_kill`

### Act 5 — Interrupt & pause (~30 s)
**Narrative:** *"TTS is bargeable. Recorder mutes during speech but still listens for a narrow interrupt set."*

| You say | Expected |
|---|---|
| "Tell me the history of the transistor" | Long reply starts streaming |
| *(mid-sentence)* "Stop" | Piper + pw-play killed instantly |
| "Pause" | TTS "Pausing.", daemon stops processing |
| "Back" *(or SIGUSR1)* | TTS "Back.", daemon resumes |

---

## 2. Failure playbook

| Symptom | Fix on stage |
|---|---|
| ARIA doesn't react to speech | Check `VAD: speech detected` in log. If absent → mic. `wpctl set-default <source_id>`. |
| Whisper transcribes garbage | You're whispering or ambient is loud. Step closer, speak normally. |
| LLM takes >5s | Ollama warming. Say "how are you" once before the real demo to warm the model in pre-flight. |
| TTS plays nothing | Sink muted or wrong default. `wpctl set-mute @DEFAULT_AUDIO_SINK@ 0`. |
| Whole pipeline frozen | `systemctl --user restart ai-agent && sleep 3` — 3-second recovery. Keep narrating. |
| Wrong tool chosen by LLM | Rephrase shorter. This is a local 8B model — own it: *"Tradeoff of fully-local: smaller model, occasional miss."* |

**Fallback demo order if time runs short:** Act 1 → Act 4 → Act 2 (pick one). Always land Act 4 — Phase 7 is the strongest engineering talking point.

---

## 3. Q&A prep — likely questions

| Question | One-liner answer |
|---|---|
| "Is anything sent to the cloud?" | No. Whisper + Ollama + Piper all run locally. The only outbound call is `web_search` via DuckDuckGo, and only when the LLM picks that tool. |
| "How fast is it?" | Whisper ≈100 ms on GPU for short utterances. LLM first-token ≈300 ms, sentence-buffered TTS hides the rest of generation. |
| "What's the memory model?" | SQLite with FTS5 virtual table on the conversations table. A background thread summarises every 20 turns. Facts are extracted passively from each turn. |
| "Why C++?" | Tight coupling to whisper.cpp, libfvad, ALSA. No GC pauses in the audio path. |
| "How does it know which tool to call?" | Ollama native function-calling — tools JSON is sent with every request, model returns `tool_calls` in the final chunk. |
| "What if the LLM hallucinates a destructive command?" | Phase 7 guard. The `run_command` path checks a denylist before the shell ever sees the string. See `executor.cpp` `checkSafety()`. |
| "Could this run offline on battery?" | Yes. Whisper and Qwen3:8B fit in 6GB VRAM. Piper is CPU-only and fast. |
| "Wake word?" | Not yet — currently always-listening. Phase 4 is openWakeWord "Hey ARIA" + 5s follow-up window. |

---

## 4. After the demo

- `git log --oneline -5` — show the two clean commits (`f36bb1a` phases 1-3, `af45cda` phase 7).
- `tokei src/` if installed, or `wc -l src/*.cpp src/*.hpp` — 3,345 LOC across 20 files.
- Offer to open any component live: `src/llm.cpp`, `src/executor.cpp`, `src/memory.cpp`.
