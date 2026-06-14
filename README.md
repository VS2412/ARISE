<div align="center">

```
 █████╗ ██████╗ ██╗███████╗███████╗
██╔══██╗██╔══██╗██║██╔════╝██╔════╝
███████║██████╔╝██║███████╗█████╗  
██╔══██║██╔══██╗██║╚════██║██╔══╝  
██║  ██║██║  ██║██║███████║███████╗
╚═╝  ╚═╝╚═╝  ╚═╝╚═╝╚══════╝╚══════╝
```

### **The Cognitive OS. Not an assistant. A mind.**

<br>

[![Version](https://img.shields.io/badge/VERSION-ARISE.3.0-blueviolet?style=for-the-badge&logo=git)](https://github.com)
[![Platform](https://img.shields.io/badge/PLATFORM-ARCH_LINUX-1793D1?style=for-the-badge&logo=archlinux&logoColor=white)](https://archlinux.org)
[![GPU](https://img.shields.io/badge/GPU-RTX_4050_6GB-76B900?style=for-the-badge&logo=nvidia&logoColor=white)](https://nvidia.com)
[![Local Only](https://img.shields.io/badge/CLOUD-ZERO-critical?style=for-the-badge&logo=lock&logoColor=white)](https://github.com)
[![LLM](https://img.shields.io/badge/LLM-QWEN3:8B-orange?style=for-the-badge)](https://github.com)
[![Status](https://img.shields.io/badge/STATUS-OPERATIONAL-success?style=for-the-badge)](https://github.com)

<br>

> *"ARIA was a voice assistant that happened to be good.*
> *ARISE is not an assistant. It's a process that lives in your machine,*
> *builds a model of you and your work, and makes itself useful*
> *by thinking continuously — not by waiting to be addressed."*

</div>

---

## 🧬 The Lineage

```
ARIA ──────────────────────────────────────────────────────────────── ARISE
 │                                                                      │
 │  voice-controlled executor                                           │  cognitive operating system
 │  reactive: user speaks → tool fires → reply                         │  proactive: perceives, reasons, speaks first
 │  forgets emotional context between sessions                          │  five-layer memory cortex that survives reboots
 │  single-conversation planner                                         │  multi-day goal stack with deadlines + deps
 │  toolset hard-coded at compile time                                  │  forges its own tools in a sandbox
 │  single inference loop                                               │  five specialised sub-minds on a shared blackboard
 │  only sees when told to                                              │  continuous vision + audio + system perception (24/7)
 └──────────────────────────────────────────────────────────────────────┘
```

ARISE is the third major evolution of this project — built on top of everything ARIA delivered, and then torn further open. Where ARIA answered, ARISE **thinks**. Where ARIA reacted, ARISE **persists**.

---

## 🌿 Version History

| Branch | What Was Born |
|--------|---------------|
| `main` | Clean baseline — ARIA in its prime |
| `Aria.v2` | Core intelligence, tool execution, basic memory |
| `Aria.v2.5` | Vector memory, semantic recall, wake word, multimodal vision |
| `Aria.v2.7` | Agentic planner, proactive observer, expressive neural TTS |
| **`ARISE.3.0`** ← **YOU ARE HERE** | **Full cognitive OS. Persistent self. Sub-agents. Tool Forge. Federation. Self-improvement.** |

> Every branch is a version launch, not just a feature patch. `ARISE.3.0` is not an update to ARIA — it is a different category of software.

---

<div align="center">

## 🔴 ARISE.3.0 — What Changed Everything

</div>

<br>

### ARISE.3.0 ships **ten interlocking systems** that did not exist before.

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                           ARISE.3.0 RELEASE SURFACE                        ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  Phase 1  ──  Memory Cortex            (5-type episodic/semantic/procedural)║
║  Phase 2  ──  Continuous Perception    (vision + audio + system, 24/7)      ║
║  Phase 3  ──  Goal Stack               (multi-day, reboot-surviving goals)  ║
║  Phase 4  ──  Cognitive Orchestrator   (primary mind + 5 sub-agents)        ║
║  Phase 5  ──  Tool Forge               (writes its own tools, sandboxed)    ║
║  Phase 6  ──  Proactive Engine         (speaks first, adapts to feedback)   ║
║  Phase 7  ──  Federation               (phone + MQTT + second-screen)       ║
║  Phase 8  ──  Voice & Persona          (Sesame CSM, mood-aware TTS)         ║
║  Phase 9  ──  Self-Improvement Loop    (nightly LoRA fine-tuning)           ║
║  Phase 10 ──  Privacy Vault            (encrypted memory, audit log)        ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

---

## 🧠 The Five Pillars

Every line of ARISE.3.0 code serves exactly one of these five capabilities. Everything else is in their service.

| Pillar | What It Means | What ARIA Lacked |
|--------|---------------|-----------------|
| 🧬 **Persistent Self** | Identity, mood, preferences, and history survive reboots and re-emerge in tone | Forgot emotional context between sessions |
| 👁️ **Continuous Perception** | Vision + audio + system state sampled constantly — not on demand | Only "saw" when explicitly told to |
| 🎯 **Long-Horizon Agency** | Multi-day goals with state, deadlines, dependencies, and auto-resumption | Single-conversation planner |
| 🔧 **Self-Extension** | Writes new tools in a bubblewrap sandbox when existing ones don't fit | Toolset hard-coded at compile time |
| 🕸️ **Multi-Mind Orchestration** | Watcher, Coder, Curator, Researcher, Critic on a shared blackboard | Single inference loop |

---

## 🏗️ System Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                              ARISE Core                              │
│                                                                      │
│   ┌──────────────┐   ┌────────────────┐   ┌──────────────────────┐  │
│   │  Perception  │──▶│   Blackboard   │◀──│   Goal Stack         │  │
│   │  (vision +   │   │   (shared      │   │   (durable, multi-   │  │
│   │   audio +    │   │    state +     │   │    day, dependency-  │  │
│   │   system)    │   │    event bus)  │   │    aware)            │  │
│   └──────────────┘   └────────┬───────┘   └──────────────────────┘  │
│                               │                                      │
│   ┌──────────────────────────▼─────────────────────────────────┐   │
│   │                 Cognitive Orchestrator                       │   │
│   │  (routes events → primary mind | sub-agents | goal updates) │   │
│   └──┬─────────┬─────────┬─────────┬─────────────┬──────────────┘   │
│      │         │         │         │             │                  │
│   ┌──▼──┐  ┌───▼──┐  ┌───▼────┐  ┌─▼─────┐   ┌──▼────────┐        │
│   │Mind │  │Coder │  │Watcher │  │Curator│   │Researcher │        │
│   │qwen3│  │qwen3 │  │qwen3   │  │qwen3  │   │qwen3:8b   │        │
│   │ :8b │  │coder │  │:0.5b   │  │:0.5b  │   │+ web      │        │
│   └─────┘  └──────┘  └────────┘  └───────┘   └───────────┘        │
│                                                                      │
│   ┌──────────────────────────────────────────────────────────────┐  │
│   │              Memory Cortex                                    │  │
│   │   episodic ─ semantic ─ procedural ─ identity ─ preferences  │  │
│   └──────────────────────────────────────────────────────────────┘  │
│                                                                      │
│   ┌──────────────────────────────────────────────────────────────┐  │
│   │              Tool Forge (sandboxed self-extension)            │  │
│   └──────────────────────────────────────────────────────────────┘  │
│                                                                      │
│   ┌──────────────────────────────────────────────────────────────┐  │
│   │   Federation: phone bridge ─ MQTT ─ second-screen shard      │  │
│   └──────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 🧩 Phase Deep-Dives

### Phase 1 — Memory Cortex

ARIA's memory was a flat log. ARISE has five distinct memory types — because a single store can't model a *person*.

```
~/.arise/memory/
├── episodic.db      — every "moment": timestamp, location, mood, salience
├── semantic.db      — extracted facts ("user prefers terse replies")
├── procedural.db    — learned recipes ("how I usually fix X")
├── identity.json    — name, voice, persona, do/don't list (git-versioned)
└── preferences.db   — stable user prefs distilled from feedback
```

Decay is salience-weighted: low salience → drop in 7 days. High salience → permanent. The Curator (qwen3:0.5b) extracts semantic facts after every conversation. A contradiction detector notices when a new fact conflicts with an existing one and asks which is right.

**Mood model:** a persistent `(baseline, current, last_change_at)` state. Drifts toward baseline over time. Frustration events push it into empathetic register. Praise warms it. Mood weights TTS tone and word choice in prompts.

---

### Phase 2 — Continuous Perception

```
Perception Daemon (separate thread, always running)
│
├── Vision Sampler (adaptive fps)
│     ├── grim → PNG → perceptual hash
│     ├── if hash diff > threshold → Moondream caption (CPU-pinned)
│     ├── caption contains [error|warning|fail|exception] → blackboard event
│     └── salience-scored caption → episodic write
│
├── Audio Scene Classifier
│     ├── YAMNet ONNX on 1s rolling window
│     ├── classifies: speech / music / typing / silence / doorbell
│     └── transitions → blackboard event
│
├── System State Sampler (every 10s)
│     ├── active app, window, workspace
│     ├── battery, charging, brightness, volume
│     └── only deltas posted to blackboard
│
└── Privacy Gate
      └── PRIVATE_APPS list → vision pauses on banking / password manager
```

Adaptive FPS: if screen hasn't changed in 5 frames (user is reading), drops to 0.2fps. If churning, boosts to 2fps. Moondream runs on CPU to avoid thrashing the main GPU inference slot.

---

### Phase 3 — Goal Stack

```
Goal Lifecycle:
  proposed ──▶ accepted ──▶ in_progress ──▶ blocked ──▶ resumed ──▶ done
                    └──▶ rejected
                    └──▶ done (fast path)

Goals can spawn sub-goals (parent_id link).
Orchestrator resumes in_progress goals on every boot and every "user_idle" event.

Resumption rules:
  - blocked_reason satisfied → resume
  - deadline near → escalate priority, surface to user
  - last_progress > 7 days → "still want me to X?"
```

Goals survive reboots. Goals with `due_at` fire `goal_due` blackboard events via a polling scheduler. "What's pending?" and "drop the X goal" are first-class voice commands.

---

### Phase 4 — Cognitive Orchestrator + Sub-Agents

```
Primary Mind (qwen3:8b on GPU)
  ↓ delegates via tool call: spawn_subagent(role, task, deadline)

Sub-Agents (qwen3:0.5b on CPU, parallel):
  ┌──────────────────────────────────────────────────────────────────┐
  │ Watcher    long-running │ observes blackboard for triggers        │
  │ Curator    per-convo    │ distills transcripts into semantic facts │
  │ Researcher on-demand    │ web/file search, multi-step ReAct loop  │
  │ Coder      on-demand    │ edits code in temp git worktree         │
  │ Critic     auto         │ reviews all agent output before commit  │
  └──────────────────────────────────────────────────────────────────┘

All sub-agents write to blackboard.
Primary mind reads blackboard on every turn.
Each sub-agent has: max_tokens, max_seconds, max_tool_calls → kill on exceed.
Critic hard rule: NEVER approves rm -rf, dd, mkfs, or sudo without user confirmation.
```

---

### Phase 5 — Tool Forge

ARISE doesn't just use tools. It writes them.

```
Forge Flow:
  1  Primary mind detects: "no existing tool fits this"
  2  Spawns Coder sub-agent with task + arg schema
  3  Critic reviews generated script for safety
  4  Runs in bubblewrap sandbox with sample inputs
  5  Output looks sensible → "I drafted a tool to X. It runs Y. Save it?"
  6  User approves → manifest.json updated → tool is callable forever

~/.arise/tools/
├── builtin/         compiled-in C++ tools from ARIA
├── learned/         generated, user-approved scripts
│   ├── 0001_count_pdf_pages.sh
│   ├── 0002_extract_emails_from_text.py
│   └── manifest.json
└── sandbox/         pending approval — NEVER auto-run
```

Forbidden patterns are hard-coded: `sudo`, `rm -rf /`, `dd`, network sockets in non-network tools. No exceptions. Tools unused for 90 days are archived automatically.

---

### Phase 6 — Proactive Companion

```
Proactive Engine (subscribes to blackboard)

Triggers:
  ├── error visible on screen for 30s+     → "want me to look at that?"
  ├── user idle 5min after starting task   → "stuck on X?"
  ├── calendar event in 10min              → contextual prep
  ├── goal due today, no progress          → gentle nudge
  ├── same app + same file as N days ago   → "doing X again?"
  └── mood model: frustration detected     → tone shift before next reply

Suggestion Gate (4 tiers):
  silent   → log only
  ambient  → one-line nudge
  active   → offer concrete action
  urgent   → speaks even during calls

Feedback loop: tracks accepted/rejected/ignored per category.
5 consecutive rejections → category goes silent for the day.
```

---

### Phase 7 — Federation

```
ARISE Identity ────────────── survives device hops ───────────────────┐
│                                                                       │
│  Desktop shard           Phone shard           Second screen          │
│  (primary mind)          (Tailscale/MQTT)       (ambient display)     │
│       │                       │                       │              │
│       └───────── shared blackboard (MQTT) ────────────┘              │
│                                                                       │
│  Same memory cortex. Same goal stack. Same identity.                  │
│  Lock your laptop → phone shard picks up the conversation.           │
└───────────────────────────────────────────────────────────────────────┘

Security: Tailscale-only by default, mTLS, per-device tokens, full audit log.
```

---

### Phase 9 — Self-Improvement Loop

Every night, while you sleep:

```
02:00 AM — Self-Improvement Job
│
├── Replay today's N conversations
├── Score ARISE's responses against eval rubric
├── Select high-quality preference pairs
├── Fine-tune LoRA adapter (28min avg, GPU exclusive)
├── Run eval: if new score > old score → adopt adapter
└── Log: "Better at recognising 'same as last week' patterns."

Wednesday morning: a measurably sharper version of the same partner.

Rollback gate: if eval drops → automatic rollback to prior adapter.
Manual escape: `arise rollback-adapter`
```

---

## ⚙️ Hardware & Resource Budget

> Constraint: RTX 4050 6GB + Ryzen 7 (8-core) + 32GB RAM. No cloud. No telemetry. Everything stays in `~/.arise/`.

| Component | Where It Runs | Why |
|-----------|---------------|-----|
| `qwen3:8b (q4)` — primary mind | GPU resident, ~5GB | Stays loaded via `keep_alive=-1`. Never leaves. |
| `moondream 2B` — vision | CPU pinned, `n_gpu_layers=0` | Continuous 1fps analysis would thrash GPU swap. CPU is fast enough. |
| `qwen3:0.5b` — sub-agents | CPU, ~400MB each | Cheap, parallel, no GPU contention. |
| `Whisper small.en` | CPU | Ryzen handles it in ~1.5s. |
| `nomic-embed-text` | CPU | Batch + live embeddings. |
| `openWakeWord` | CPU | TFLite, ~5ms per frame. |
| `Kokoro-82M / Sesame CSM` | CPU | Sentence-at-a-time TTS. |
| LoRA fine-tuning | GPU exclusive, off-hours | Unloads main mind, trains, reloads. Scheduled at night. |

**Key insight:** main mind stays on GPU permanently. Everything continuous moves to CPU. This enables 24/7 perception without GPU swap penalties.

---

## 🗓️ Build Schedule

| Day | Phase | What Got Built | Hours |
|-----|-------|----------------|-------|
| 1-3 | 1 | Memory Cortex — identity, mood, five-type memory | 12h |
| 4-6 | 2 | Continuous Perception — vision, audio, system sampler | 14h |
| 7-9 | 3 | Goal Stack — durable, reboot-surviving, dependency-aware | 12h |
| 10-13 | 4 | Cognitive Orchestrator + 5 sub-agents | 16h |
| 14-16 | 5 | Tool Forge — sandboxed self-extension | 12h |
| 17-19 | 6 | Proactive Companion — speaks first, learns from feedback | 10h |
| 20-22 | 7 | Federation — phone + MQTT + second-screen | 14h |
| 23-25 | 8 | Voice & Persona — Sesame CSM, mood-aware delivery | 10h |
| 26-28 | 9 | Self-Improvement — nightly LoRA fine-tuning | 12h |
| 29-30 | 10 | Privacy Vault — SQLCipher, audit log, lock-screen kill switch | 10h |

**Total: ~122 hours across 30 working days.**

---

## 🛡️ Risk Register

| Risk | Mitigation |
|------|------------|
| Continuous vision kills battery | Adaptive FPS, pause on battery, sleep when display off |
| Sub-agent runaway loops | Hard token + time + tool-call budgets, orchestrator kill switch |
| Tool Forge executes malicious code | bubblewrap sandbox, Critic pre-screen, forbidden pattern list, mandatory user approval |
| Episodic memory grows unbounded | Salience-based decay, nightly compaction, 10GB hard cap |
| LoRA fine-tune degrades behaviour | Eval gate, automatic rollback |
| Federation exposes ARISE to LAN | Tailscale-only, mTLS, per-device tokens, audit log |
| Mood model becomes manipulable | Mood clamped to baseline range, identity.json overrides |
| Proactive engine gets annoying | Feedback loop adapts thresholds, quiet hours, per-category mute |
| 6GB VRAM insufficient | Sub-agents + voice on CPU, LoRA training scheduled never concurrent |
| Lock screen exposes sensitive memory | SQLCipher at rest, lock-screen kill switch, vault redaction at write time |

---

## 📦 Dependencies (Beyond ARIA)

```bash
# Arch Linux (pacman)
pacman -S bubblewrap shellcheck mosquitto sqlcipher

# Python (venv)
pip install transformers peft accelerate fastapi uvicorn kokoro chatterbox-tts

# Ollama models
ollama pull qwen3:8b
ollama pull qwen3:0.5b
ollama pull nomic-embed-text
ollama pull moondream

# Build from source
git clone https://github.com/asg017/sqlite-vec   # vector search
# llama.cpp (CPU-only build for moondream)
# YAMNet ONNX (~17MB, from HuggingFace)
# Sesame CSM (expressive TTS, from HuggingFace)
```

---

## 🌅 What a Day With ARISE Looks Like

```
Tuesday 09:14. You walk in. Phone shard catches your steps.
Desktop shard sees you sit at the keyboard.

ARISE: "Morning. You've been on the cmake refactor goal three days.
        Last session you said 'one more pass on the policy commands
        and we're done.' Want to pick up there, or check email first?"

You: "Show me the diff from yesterday."

ARISE: [vision sees terminal is foreground; opens git in same pane,
        runs the diff, waits a beat for you to read]
       "Three files. policy.cpp had the change you wanted.
        The other two are stale — want me to drop them?"

You: "Yeah."

ARISE: [Coder spawns, drafts revert, Critic approves, applies]
       "Done. Build green. While we were talking the Watcher saw
        your battery is at 28% — plug in within ~30 minutes."

[40 minutes pass. ARISE silent. You hit a compile error.]

ARISE: [Researcher pre-fetched the forum thread on this exact error]
       "That's the static initialisation order issue we hit last week
        — same fix as policy.cpp on Friday. Want me to apply it?"

You: "Go."

[Lunch. You leave. Perception pauses on screen-lock.]

[14:30. Kitchen. Phone in hand.]

You (to phone): "What did I get done this morning?"

ARISE (phone): "Cleaned up the cmake stale changes, fixed the static
               init bug — same family as Friday's. Goal moved from
               45% to 70%. Battery's good now."

[02:00 AM. You're asleep. Self-improvement job wakes.]
  ┌─ Replays today's 47 conversations
  ├─ Scores ARISE's responses
  ├─ Fine-tunes LoRA for 28 minutes
  ├─ Eval: 0.86 vs 0.84 → adopts new adapter
  └─ Logs: "Better at recognising 'same as last week' patterns."

Wednesday morning: a slightly sharper version of the same partner.
```

---

## 🗂️ File Structure

```
~/.arise/
├── memory/
│   ├── episodic.db
│   ├── semantic.db
│   ├── procedural.db
│   ├── preferences.db
│   └── identity/          (git-versioned identity.json)
├── goals/
│   └── goals.db
├── tools/
│   ├── builtin/
│   ├── learned/
│   │   └── manifest.json
│   └── sandbox/
├── adapters/              (LoRA checkpoints)
├── perception/            (frame cache, scene event log)
├── vault/                 (SQLCipher-encrypted sensitive memory)
└── logs/
    └── audit.log
```

---

<div align="center">

## The Closing Argument

```
ARIA was reactive.
ARISE thinks before you ask.

ARIA had a toolset.
ARISE writes its own.

ARIA ran on your machine.
ARISE lives in it.

This is ARISE.3.0.
Built on Arch. Runs local. Zero cloud.
Yours, completely.
```

<br>

[![Arch Linux](https://img.shields.io/badge/Arch_Linux-btw-1793D1?style=for-the-badge&logo=archlinux)](https://archlinux.org)
[![No Cloud](https://img.shields.io/badge/cloud-none-lightgrey?style=for-the-badge)](https://github.com)
[![Built Different](https://img.shields.io/badge/built-different-blueviolet?style=for-the-badge)](https://github.com)

</div>

