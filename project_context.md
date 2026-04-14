# ARIA — Project Context

## 1. Identity

- **Name:** ARIA — always-on voice AI assistant
- **Platform:** Arch Linux, niri WM, Wayland, PipeWire
- **Language:** C++20
- **Branch:** `Aria.v2`
- **Size:** ~3,345 LOC across 20 source files
- **Binary:** `build/ai-agent`
- **Runtime:** systemd user service `ai-agent.service`
- **Logs:** `~/.ai-agent.log` + journal
- **DB:** `~/.aria_memory.db` (SQLite)

## 2. Hardware / Host Dependencies

| Dependency | Version / Path | Purpose |
|---|---|---|
| whisper.cpp | `/home/Aurelius/Documents/AdoVs/whisper.cpp` (CUDA build) | Local STT |
| Whisper model | `models/ggml-small.en.bin` | English small model |
| Ollama | `localhost:11434`, model: `qwen3:8b` (via `ARIA_MODEL`) | Reasoning + tool calls |
| Piper TTS | `~/.local/share/piper/en_US-lessac-medium.onnx` | Streaming synthesis |
| libfvad | pkg-config | WebRTC VAD |
| niri | `niri msg --json` | Window mgmt IPC |
| wl-clipboard, wtype | Wayland tooling | Clipboard / keyboard injection |
| wpctl, brightnessctl, playerctl | | Volume, brightness, media |
| grim, tesseract | | Screenshots, OCR |
| pacman (never apt) | | Package manager |

## 3. Pipeline (Data Flow)

```
parec (ALSA)
  → libfvad VAD (recorder.cpp, onset=10 trail=25 frames)
  → /tmp/aria_speech.wav → speechQueue (mutex + cv)
  → processor thread
    ├─ Transcriber::transcribe (whisper.cpp GPU CUDA)
    ├─ isInterruptCommand? → tts.interrupt() short-circuit
    ├─ extractFacts → memory.setFact (passive learning)
    ├─ classifyIntent (regex fast-path) → AgentAction
    │    if non-empty → executeAction → feedback via tts.speak
    └─ else: Context::capture → direct answer or streaming LLM
         ├─ LLM::thinkStreaming (Ollama NDJSON)
         │    ├─ <think> tag stripped in-flight
         │    └─ text deltas → tts.feedChunk (sentence-buffered)
         ├─ if tool_calls in final chunk:
         │    ├─ single action → executeAction
         │    └─ run / multi-step → ReAct loop (max 5 steps)
         │         └─ LLM::reactStreaming with observation
         └─ maybeSummarize (every 20 turns, detached thread)
  → tts playback thread: piper --output_file /dev/stdout --quiet | pw-play -
```

## 4. Core Components

### 4.1 Daemon (`src/daemon.cpp` ~430 LOC)
- Single processor thread consuming the speech queue.
- Owns TimerManager instance; routes `timer_*` and `recall_memory` actions via `executeAction` lambda before falling through to `Executor::execute()`.
- `maybeSummarize()` spawns detached summarizer thread when `lastConversationId - lastSummarizedId >= 20`.
- Main thread: SIGUSR1 pause toggle, SIGINT/SIGTERM clean shutdown.

### 4.2 Recorder (`src/recorder.*`)
- ALSA capture + WebRTC VAD. Tuned: ONSET_FRAMES=10 (200ms), TRAIL_FRAMES=25 (500ms).
- `mute()`/`unmute()` brackets every TTS call — prevents self-hearing.

### 4.3 Transcriber (`src/transcriber.*`)
- whisper.cpp small.en on CUDA0, ~487 MB model.

### 4.4 Intent (`src/intent.*`)
- Regex fast-path for "open X", "volume up", "workspace 2", "screenshot", etc. No network hit.
- Returns empty `AgentAction` → daemon falls through to LLM.

### 4.5 LLM (`src/llm.*` ~870 LOC)
- Ollama native tool-calling (`/api/chat` with `tools` array), `think:false`, `stream:true`.
- `thinkStreaming` / `reactStreaming` — NDJSON chunk parser with cross-chunk `<think>` state machine.
- `summarize()` — one-shot, no history, no tools; used by background summarizer.
- Rolling history: `MAX_HISTORY = 16`.
- Batch `think()`/`react()` wrap streaming variants with null callback.

### 4.6 Executor (`src/executor.*` ~445 LOC)
- Maps `AgentAction{type, args}` to shell commands.
- `shellCapture()` — popen with 2000-char truncation for LLM ReAct observations.
- `kAppMap` resolves friendly names ("browser", "vs code", "files") to binaries.
- Timer actions return sentinel `"__TIMER__"` (never reached — daemon intercepts).
- **Safety layer (Phase 7):** `checkSafety()` three-tier guard (`Allow`/`Caution`/`Deny`) gates `run_command`. `proc_kill` refuses PID ≤ 1 and critical names via `isCriticalProc()`.

### 4.7 Context (`src/context.*`, `src/screen.*`)
- Snapshot of: active window/app (niri JSON), clipboard (wl-paste), screen OCR (tesseract via grim).

### 4.8 Memory (`src/memory.*` 309 LOC)
- SQLite with schema: `conversations`, `user_facts`, `summaries`, `conversations_fts` (FTS5 external-content).
- AI/AD/AU triggers keep FTS in sync with conversations.
- `search(query, limit)` — tokens quoted and OR'd into FTS5 MATCH.
- `getSummary()` returns facts + last 3 summaries + last 6 raw turns — injected into every LLM system prompt.
- Idempotent FTS rebuild on startup if `docsize` count lags conversation count.

### 4.9 TTS (`src/tts.*` 251 LOC)
- Streaming API: `startStream()`, `feedChunk(text)`, `endStream()`.
- Sentence boundaries: `. `, `? `, `! `, `\n`, or 120+ chars.
- Background playback thread: `piper --output_file /dev/stdout --quiet | pw-play -`.
- `interrupt()` kills active piper + pw-play processes.

### 4.10 TimerManager (`src/timer.*` 85 LOC)
- Background thread polls every 1s. Fires callback on `now >= fireAt`.
- On fire: `recorder.mute()` → `tts.speak("Timer done: " + label)` → `notify-send` → `recorder.unmute()`.

## 5. LLM Tool Surface (27 tools)

| Category | Tools |
|---|---|
| Core | `open_application`, `run_command`, `open_url`, `task_done` |
| Desktop control | `set_volume`, `set_brightness`, `media_control`, `take_screenshot`, `switch_workspace`, `close_window`, `type_text`, `read_clipboard`, `write_clipboard`, `send_notification` |
| Files | `read_file`, `write_file`, `search_files`, `list_directory` |
| Windows | `window_action`, `list_windows`, `focus_window` |
| Processes | `list_processes`, `kill_process` |
| Timers | `set_timer`, `list_timers`, `cancel_timer` |
| Web | `web_search` |
| System | `system_info` |
| Memory | `recall_memory` |

Tool → action mapping lives in `toolToAction()` in `llm.cpp`. Action → shell in `Executor::execute()`.

## 6. Memory Schema

```sql
CREATE TABLE conversations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT DEFAULT (datetime('now','localtime')),
    role TEXT NOT NULL,
    content TEXT NOT NULL
);

CREATE TABLE user_facts (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at TEXT DEFAULT (datetime('now','localtime'))
);

CREATE TABLE summaries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT DEFAULT (datetime('now','localtime')),
    content TEXT NOT NULL,
    turn_start INTEGER,
    turn_end INTEGER
);

CREATE VIRTUAL TABLE conversations_fts USING fts5(
    content, content='conversations', content_rowid='id'
);
-- + triggers: conversations_ai / conversations_ad / conversations_au
```

**Passively learned facts:** `user_name`, `location`, `job`, `preference`, `current_project`.

## 7. Key Invariants

- TTS must be bracketed by `recorder.mute()` / `recorder.unmute()` to avoid feedback.
- Interrupt commands (`stop`, `cancel`, `shut up`, `quiet`, `enough`) short-circuit to `tts.interrupt()`.
- Noise filter: empty action + wordCount ≤ 2 = discard.
- Piper output MUST be WAV (`--output_file /dev/stdout`), NOT raw PCM — `pw-play` requires a container.
- Tool calls in Ollama streaming only arrive in the final `done:true` chunk.
- FTS5 external-content `NOT EXISTS` checks are unreliable — use `docsize` count to decide rebuild.
- Timer and `recall_memory` actions MUST be intercepted by daemon before reaching executor.
- SQLite opens in default serialized mode — safe to share `db_` across threads (summarizer thread, processor thread).
- Safety guard runs on the raw command string pre-shell; patterns are case-insensitive substring matches — tune `kDenyPatterns` / `kCautionPatterns` / `kCriticalProcs` in `executor.cpp` if false positives surface.

## 8. Runtime Controls

| Signal / Command | Effect |
|---|---|
| `SIGUSR1` | Toggle pause (ariaActive) |
| `SIGINT` / `SIGTERM` | Clean shutdown (recorder.stop + processor.join) |
| `ARIA_MODEL=<name>` | Override Ollama model |
| `systemctl --user restart ai-agent` | Restart service |
| `journalctl --user -u ai-agent -f` | Live logs |

## 9. Build

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent
cmake --build build -j$(nproc)
```

CMakeLists.txt hardcodes whisper.cpp at `/home/Aurelius/Documents/AdoVs/whisper.cpp`. Links: `whisper`, `ggml`, `CURL`, `nlohmann_json`, `SQLite3`, `libfvad`. `-Wall -Wextra -O2`, `_POSIX_C_SOURCE=200809L`.

## 10. File Layout

```
src/
├── main.cpp          42   signals + daemon bootstrap
├── daemon.cpp       ~430  orchestrator, TimerManager, summarizer
├── daemon.hpp        11
├── recorder.cpp     143   ALSA + fvad
├── recorder.hpp      39
├── transcriber.cpp   91   whisper.cpp wrapper
├── transcriber.hpp   14
├── intent.cpp       ~80   regex fast-path classifier
├── intent.hpp         6
├── llm.cpp         ~870   Ollama streaming + 27 tools + summarize
├── llm.hpp           81
├── executor.cpp    ~445   25+ action handlers + safety guard
├── executor.hpp      11
├── context.cpp     ~110   niri/wl-paste/OCR capture
├── context.hpp       13
├── screen.cpp        28   tesseract OCR
├── screen.hpp         7
├── memory.cpp       309   SQLite + FTS5 + summaries
├── memory.hpp        45
├── tts.cpp          251   Piper streaming + sentence buffer
├── tts.hpp           46
├── timer.cpp         85   TimerManager fire loop
├── timer.hpp         37
├── logger.cpp       ~30
└── logger.hpp         8
```

## 11. Phase Progress

| Phase | Status | Scope |
|---|---|---|
| 1 — Streaming Pipeline | ✅ Done | Ollama NDJSON streaming, sentence-buffered TTS, tightened VAD |
| 2 — Richer Tools | ✅ Done | +14 tools: files, windows, processes, timers, web, sysinfo |
| 3 — Deep Memory | ✅ Done | FTS5, summaries (20-turn threshold), recall_memory, enhanced facts |
| 7 — Security Hardening | ✅ Done | 3-tier safety guard (Deny/Caution/Allow) on run_command + proc_kill |
| 6 — Latency Polish | Pending | Cached OCR, parallel context, whisper no_context |
| 4 — Wake Word | Pending | openWakeWord sidecar + follow-up window state machine |
| 5 — Conversational Quality | Pending | ask_user tool, richer prompts, tone detection |

Full plan: `.claude/plans/wild-percolating-fiddle.md`.
