# Presentation Slides – AI Assistant Overview

---
## Slide 1: Title
**AI Assistant – Voice‑First Linux Agent**

*Speaker Notes (Presenter 1):*
- Greet the audience, introduce the team (three members).
- Briefly state the problem: hands‑free interaction with the OS.
- Mention that the demo runs as a native kernel‑level daemon.

---
## Slide 2: Motivation & Use‑Cases
- Hands‑free productivity (coding, browsing, system admin).
- Accessibility for users with limited mobility.
- Continuous personal assistant that learns over time.

*Speaker Notes (Presenter 2):*
- Highlight real‑world scenarios (e.g., “Start a timer while cooking”).
- Emphasize privacy – runs locally, no cloud data.

---
## Slide 3: High‑Level Architecture
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

*Speaker Notes (Presenter 1):*
- Walk through each block, explain responsibilities.
- Mention that Daemon ties everything together and handles signals.

---
## Slide 4: Core Components
| Component | Source File(s) | Responsibility |
|-----------|----------------|----------------|
| Daemon | `daemon.cpp/hpp` | Orchestrates pipeline, signal handling |
| Recorder | `recorder.cpp/hpp` | Captures microphone audio to WAV |
| Transcriber | `transcriber.cpp/hpp` | Whisper‑based speech‑to‑text |
| LLM | `llm.cpp/hpp` | Streaming inference, summarization |
| Executor | `executor.cpp/hpp` | Safe shell command execution |
| Memory | `memory.cpp/hpp` | SQLite persistence, FTS5 search |
| Intent | `intent.cpp/hpp` | Rule‑based short‑command detection |
| Timer | `timer.cpp/hpp` | User‑requested timers, notifications |
| TTS | `tts.cpp/hpp` | Piper streaming TTS |
| Context | `context.cpp/hpp` | Capture active app, window, clipboard |

*Speaker Notes (Presenter 2):*
- Briefly describe each module, highlight C++20 features.

---
## Slide 5: Data Flow Walkthrough
1. **Audio Capture** – Recorder writes `/tmp/aria_speech.wav` and pushes path to queue.
2. **Transcription** – Whisper converts audio → text.
3. **Fact Extraction** – Parses name, location, job, stores in Memory.
4. **Intent Check** – Quick rule‑based commands (timer, clipboard).
5. **LLM Reasoning** – Sends text + context + recent memory to LLM.
6. **Action Execution** – Executor runs shell commands or timer manager handles timers.
7. **Response** – Streamed to TTS, spoken back to user.
8. **Memory Update** – Persists utterances, facts, and summaries.

*Speaker Notes (Presenter 3):*
- Walk through a sample interaction (“Set a 5‑minute timer”).
- Emphasize the ReAct loop for multi‑step tasks.

---
## Slide 6: Memory & Summarization
- **Conversations Table** – Stores role (user/assistant) and content.
- **User Facts** – Persistent key‑value store (name, location, etc.).
- **Summaries** – Periodic compression of older turns (every ~20 turns).
- **Full‑Text Search** – FTS5 virtual table for fast retrieval.

*Speaker Notes (Presenter 1):*
- Explain how long‑term memory enables context‑aware answers.

---
## Slide 7: ReAct Loop (Reason‑Act‑Observe)
- LLM can emit actions (`run`, `timer_set`, `recall_memory`).
- Daemon executes action, captures observation, feeds back to LLM.
- Loop repeats up to 5 steps, then finalizes.

*Speaker Notes (Presenter 2):*
- Show a multi‑step example (search web → read result → summarize).

---
## Slide 8: Build & Run
```bash
# Install dependencies (Ubuntu example)
sudo apt-get install libsqlite3-dev libcurl4-openssl-dev libpiper-dev libffmpeg-dev

mkdir -p build && cd build
cmake ..
make -j$(nproc)
./ai-agent
```
- Environment variables:
  - `HOME` – log file location.
  - `ARIA_MODEL` – custom LLM path.
  - `ARIA_DB` – custom SQLite DB path.

*Speaker Notes (Presenter 3):*
- Mention that the binary is lightweight (< 20 MB) and runs in the background.

---
## Slide 9: Demo (Live)
- Show the assistant listening, responding, setting a timer, and recalling a fact.
- Highlight voice‑only interaction.

*Speaker Notes (All presenters):*
- Coordinate the live demo, ensure microphone works.

---
## Slide 10: Future Work & Roadmap
- Add multimodal input (screen OCR, webcam).
- Plug‑in architecture for custom actions.
- Better privacy controls (local encryption of memory).
- Port to other OSes (macOS, Windows via WSL).

*Speaker Notes (Presenter 1):*
- Invite questions, thank the audience.

---
*Generated on 2026‑04‑14.*
