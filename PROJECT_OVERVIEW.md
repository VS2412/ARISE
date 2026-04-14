# AI Assistant Project Overview

## Introduction
The **AI Assistant** is a native Linux kernel‑level agent that continuously listens to the user's speech, processes it with a large language model (LLM), and performs actions on the host system. It is built in modern C++20 and uses SQLite for persistent memory, libcurl for networking, and the **Piper** TTS engine for speech output.

## High‑Level Architecture
```
+-------------------+      +-------------------+      +-------------------+
|   Audio Capture   | ---> |   Speech          | ---> |   Intent & LLM    |
|   (Recorder)      |      |   Transcriber     |      |   Reasoning       |
+-------------------+      +-------------------+      +-------------------+
        |                         |                         |
        v                         v                         v
+-------------------+      +-------------------+      +-------------------+
|   Memory (SQLite) | <--- |   Fact Extraction | <--- |   Action Executor |
+-------------------+      +-------------------+      +-------------------+
        ^                         ^                         ^
        |                         |                         |
+-------------------+      +-------------------+      +-------------------+
|   TTS (Piper)    | <--- |   Timer Manager   | <--- |   Daemon (Main)   |
+-------------------+      +-------------------+      +-------------------+
```

- **Daemon** (`daemon.cpp/hpp`): Core orchestrator. Sets up signal handling, launches the audio recorder, and runs the processing loop.
- **Recorder** (`recorder.cpp/hpp`): Captures microphone audio to a temporary WAV file and pushes the file path onto a thread‑safe queue.
- **Transcriber** (`transcriber.cpp/hpp`): Wraps the Whisper model to convert audio to text.
- **LLM** (`llm.cpp/hpp`): Interfaces with a local or remote LLM (default `qwen3:8b`). Provides `thinkStreaming`, `reactStreaming`, and `summarize`.
- **Executor** (`executor.cpp/hpp`): Executes shell commands safely and returns captured output.
- **Memory** (`memory.cpp/hpp`): Persistent SQLite database storing:
  - Conversation turns (`conversations` table)
  - User facts (`user_facts` table)
  - Summaries of older conversation windows (`summaries` table)
  - Full‑text search via FTS5 virtual table.
- **Intent** (`intent.cpp/hpp`): Simple rule‑based classifier for common commands (e.g., timer control, clipboard queries).
- **TimerManager** (`timer.cpp/hpp`): Handles user‑requested timers, mutes the recorder while speaking, and sends desktop notifications.
- **TTS** (`tts.cpp/hpp`): Streams text‑to‑speech using Piper, supports chunked streaming for low‑latency responses.
- **Context** (`context.cpp/hpp`): Captures current active application, window title, clipboard, and screen text for context‑aware queries.
- **Screen** (`screen.cpp/hpp`): Utility for reading on‑screen text (used by the LLM for richer context).
- **Logger** (`logger.cpp/hpp`): Simple file‑based logger with timestamps.

## Data Flow
1. **Audio Capture** – `Recorder` writes a WAV file and notifies the processing thread.
2. **Transcription** – `Transcriber` converts WAV → plain text.
3. **Fact Extraction** – `extractFacts` parses the text for user name, location, job, etc., storing them in `Memory`.
4. **Intent Classification** – `classifyIntent` checks for short commands (timer, clipboard, app info).
5. **LLM Reasoning** – If no direct intent, the text is sent to the LLM with the current `Context` and recent memory summary.
6. **Action Execution** – LLM may return actions (e.g., `run`, `timer_set`). `Executor` runs shell commands; `TimerManager` handles timers.
7. **Response** – LLM output is streamed to `TTS`, which speaks the answer while the recorder is muted.
8. **Memory Update** – All user utterances, assistant replies, and extracted facts are persisted.
9. **Summarization** – Every ~20 turns the daemon spawns a background thread to summarize older conversation chunks and store them as summaries.

## Build & Run
```bash
# Install dependencies (Ubuntu example)
sudo apt-get install libsqlite3-dev libcurl4-openssl-dev libpiper-dev libffmpeg-dev

# Build
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# Run
./ai-agent
```
The binary reads environment variables:
- `HOME` – location for the log file (`$HOME/.ai-agent.log`).
- `ARIA_MODEL` – optional path to a custom LLM model.
- `ARIA_DB` – optional path to the SQLite DB (defaults to `$HOME/.aria_memory.db`).

## Usage Highlights
- **Voice‑first interaction** – No keyboard needed; just speak.
- **Persistent memory** – Remembers facts across sessions.
- **ReAct loop** – Multi‑step reasoning with observation/action cycles.
- **Timer & notification integration** – Uses `notify-send` for desktop alerts.
- **Extensible** – Add new `AgentAction` types in `executor.cpp` and handle them in the daemon.

## Presentation Script (see `PRESENTATION_SLIDES.md`)
The accompanying slide deck outlines the same architecture with speaker notes for a 10‑15 minute demo by three presenters.

---
*Generated on 2026‑04‑14.*
