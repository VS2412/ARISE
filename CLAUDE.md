# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ARIA is a voice-controlled AI desktop agent for Arch Linux with niri WM. It continuously listens for speech, transcribes it locally via whisper.cpp, classifies intent (regex fast-path or LLM fallback via Ollama), executes system actions, and responds with Piper TTS. Written in C++20.

## Build

```bash
cd build && cmake .. && make -j$(nproc)
```

The binary is `build/ai-agent`. Run it directly; it logs to `~/.ai-agent.log`. Send SIGUSR1 to toggle pause, SIGINT/SIGTERM to shut down.

## Dependencies

- **whisper.cpp** — hardcoded at `/home/Aurelius/Documents/AdoVs/whisper.cpp` (model: `ggml-small.en.bin`). Must be built separately (`build/src/libwhisper.so`, `build/ggml/src/libggml.so`).
- **Ollama** — must be running at `localhost:11434` with `llama3.1` model pulled.
- **Piper TTS** — model at `~/.local/share/piper/en_US-lessac-medium.onnx`.
- **System packages**: `curl`, `nlohmann-json`, `sqlite3`, `libfvad`, `niri`, `wl-clipboard`, `wtype`, `wireplumber` (wpctl), `brightnessctl`, `playerctl`, `grim`, `tesseract` (for OCR).
- Package manager is **pacman**. Never use apt.

## Architecture

The pipeline is: **Recorder -> Transcriber -> Intent/LLM -> Executor -> TTS**, orchestrated by the Daemon.

- **daemon.cpp** — Central orchestrator. Spawns the Recorder, runs a processing thread that dequeues transcribed speech and routes it through intent classification then execution. Handles pause/resume via SIGUSR1.
- **recorder** — ALSA capture with libfvad-based Voice Activity Detection. Detects speech onset/offset and writes WAV segments to `/tmp/aria_speech.wav`, calling a callback with the path.
- **transcriber** — Wraps whisper.cpp for local speech-to-text.
- **intent.cpp** — Regex-based fast-path classifier. Returns an `AgentAction` directly for known commands (open app, volume, media, workspace, screenshot, etc.) without hitting the LLM. Returns empty action to fall through to LLM.
- **llm.cpp** — Ollama API client using native tool-calling (not JSON-in-prompt). Sends system prompt with current context (active app, clipboard, screen OCR, memory summary). Parses tool calls into `AgentAction` or returns conversational speech. Maintains a rolling 8-message history.
- **executor.cpp** — Maps `AgentAction{type, param}` to system commands. Has an app name resolution table (`kAppMap`). Supports: open, run, type, workspace, close, volume, brightness, media, screenshot, clipboard, notify, url, and chained sequences.
- **context.cpp** — Captures system state: active window/app via `niri msg --json`, clipboard via `wl-paste`, screen text via OCR.
- **screen** — OCR of current screen content.
- **memory.cpp** — SQLite DB at `~/.aria_memory.db`. Stores conversation history and key-value facts (e.g. user's name). Provides `getSummary()` for LLM context injection.
- **tts** — Piper TTS wrapper. Supports interrupt (kills active speech process).
- **logger** — File logger to `~/.ai-agent.log`.

## Key Data Types

- `AgentAction{type, param}` — the universal action format passed between intent/LLM and executor.
- `LLMResponse{speech, action}` — LLM output: either a conversational reply, an action, or both.
- `LLMContext` — system state snapshot passed to the LLM system prompt.
- `SystemContext` — raw captured context (active window, app, clipboard, screen text).

## Two-Phase Intent Resolution

1. `classifyIntent()` tries regex matching first (no network, instant).
2. If it returns an empty action, the daemon sends the text to the LLM with full system context for reasoning and tool-calling.

This means simple commands like "open firefox" or "volume up" never hit Ollama.
