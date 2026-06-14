# ARISE — Phase Notes & Test Cheat Sheet

Living reference doc. One section per phase. Each section lists what shipped,
how to verify it, and copy-pasteable test commands from basic to advanced.

The full roadmap lives at `.claude/plans/arise.md`. This file is the day-to-day
operations manual.

**Convention:** every command assumes the alias

```bash
alias arise=/home/Aurelius/Documents/AdoVs/ai-agent/build/arise/arise
```

is set, and that the build is fresh:

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && make -j$(nproc)
```

ARISE stores everything under `~/.arise/` by default. Override with
`ARISE_ROOT=/tmp/sandbox` for a throwaway run.

---

## Phase 1 — Memory Cortex, Identity, Mood (shipped 2026-05-01, commit `2999b71`)

### What's new

| Component | What it does |
|---|---|
| **Episodic DB** | Time-stamped log of what happened. Salience-tiered decay: `<0.3` → 7d, `<0.7` → 90d, `≥0.7` → permanent. Fields: kind, summary, payload, salience, mood_at, refs, decay_at, embedding. |
| **Semantic DB** | Subject-predicate-object facts with confidence. Contradiction-aware upsert: weaker contradictions are rejected; stronger ones overwrite in place; matching reinforces confidence. |
| **Procedural DB** | Recipes that have worked. Goal pattern → step JSON + success/failure counts. |
| **Preferences DB** | Stable user-tuning knobs (key/value/weight). |
| **Identity store** | JSON persona file inside an auto-managed git repo at `~/.arise/identity/.git`. Every change is a commit. |
| **Mood model** | Two-axis valence/arousal (-1..+1 each) persisted to `mood.json`. Decays exponentially toward zero with configurable half-life (default 8h). |
| **Hybrid recall** | FTS5 + vec0 (sqlite-vec) blended with Reciprocal Rank Fusion + recency exp decay. Auto-falls-back to FTS-only if `vec0.so` missing. |
| **Embedding client** | Ollama `/api/embeddings` (default `nomic-embed-text`, 768-dim) with on-disk cache keyed by `(model, fnv64(text), len)`. L2-normalised output. |
| **`arise` CLI** | Single binary at `build/arise/arise` exposing every piece above. |
| **8 GoogleTests** | Smoke, embed-cache dedup, salience purge, contradiction resolution, mood persistence, mood decay, recall ranking, identity round-trip + git versioning. |

### On-disk layout after `arise init`

```
~/.arise/
├── memory/
│   ├── episodic.db       (events + FTS5 + vec0)
│   ├── semantic.db       (facts + FTS5)
│   ├── procedural.db
│   ├── preferences.db
│   └── mood.json
├── identity/
│   ├── .git/             (auto-managed, persona history)
│   └── identity.json
├── cache/
│   └── embeddings.db     (Ollama embed cache; populated on first recall)
├── logs/
└── runtime/
```

### Test commands — basic

```bash
# 0 — sandbox so this can't touch your real ~/.arise
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT"

# 1 — initialise
arise init
ls -la "$ARISE_ROOT"

# 2 — record some events
arise mem record --kind conversation_turn --summary "user asked about the weather"
arise mem record --kind build_failure     --summary "cmake fails to link libfvad" --mood frustrated --salience 0.8
arise mem record --kind casual            --summary "had pizza for dinner"
arise mem record --kind summary           --summary "finalised Q2 plan with team" --salience 0.95

arise mem dump --recent 10

# 3 — record some facts
arise mem fact --subject user --predicate likes --object coffee --confidence 0.9
arise mem fact --subject user --predicate has_dog --object Mochi --confidence 1.0
arise mem facts --subject user
```

### Test commands — recall (the interesting one)

```bash
arise mem recall "build error cmake"      # should top-rank the libfvad event
arise mem recall "food I ate"             # should top-rank pizza
arise mem recall "team planning"          # should top-rank Q2 summary
arise mem recall "dog name"               # should top-rank the Mochi fact
```

If Ollama is up with `nomic-embed-text`, the search uses both keywords and
semantic embeddings. Without it, it falls back to FTS5 keyword-only —
still works, just less fuzzy.

### Test commands — contradiction resolution

```bash
# vim wins first
arise mem fact --subject user --predicate favourite_editor --object vim --confidence 0.7
arise mem facts --subject user --predicate favourite_editor

# weaker contradiction → rejected (look for "rejected lower-confidence" in log)
arise mem fact --subject user --predicate favourite_editor --object emacs --confidence 0.6
arise mem facts --subject user --predicate favourite_editor    # still vim

# stronger contradiction → overwrites
arise mem fact --subject user --predicate favourite_editor --object emacs --confidence 0.95
arise mem facts --subject user --predicate favourite_editor    # now emacs

# matching upsert reinforces (confidence creeps toward 1.0)
arise mem fact --subject user --predicate favourite_editor --object emacs --confidence 0.5
arise mem facts --subject user --predicate favourite_editor    # confidence ≥ 0.95 still
```

### Test commands — mood

```bash
arise mem mood show
arise mem mood nudge 0.6 0.5 excited     # +valence, +arousal
arise mem mood show
arise mem mood nudge -0.8 0.3 frustrated
arise mem mood show
arise mem mood baseline calm             # change resting state
arise mem mood tick                      # apply decay toward zero now
arise mem mood show
```

Survives reboot — kill ARISE, reopen, mood is preserved (it's in
`memory/mood.json`).

### Test commands — identity (with git versioning)

```bash
arise identity show
arise identity set --name "Aria" --persona "Sharp, dry, helps Aurelius ship"
arise identity set --add-do "answer in under 3 sentences when possible"
arise identity set --add-dont "apologise unprompted"
arise identity show

# every set is a commit
git -C "$ARISE_ROOT/identity" log --oneline
git -C "$ARISE_ROOT/identity" show HEAD
```

### Test commands — migrate ARIA's existing memory

If you have `~/.aria_memory.db` from ARIA:

```bash
unset ARISE_ROOT             # use real ~/.arise
arise import-aria --dry-run  # preview, no writes
arise import-aria            # actually migrate
arise mem dump --recent 5    # should now contain old ARIA conversations
arise mem facts --subject user
```

Override source: `arise import-aria --source /path/to/some.db`.

### Test commands — maintenance & introspection

```bash
arise mem purge                        # delete decayed events now
du -sh "$ARISE_ROOT/memory/"*          # see DB sizes
ls -la "$ARISE_ROOT/cache/"            # embedding cache, if Ollama is up
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build
ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 8`. Test #2 (embed cache)
auto-skips if Ollama is unreachable.

### Knobs (env vars)

| Env var | Default | Effect |
|---|---|---|
| `ARISE_ROOT` | `~/.arise` | Where everything lives. |
| `ARIA_OLLAMA_URL` | `http://127.0.0.1:11434` | Ollama endpoint for embeddings. |
| `ARIA_EMBEDDING_MODEL` | `nomic-embed-text` | Embedding model name. |
| `ARIA_EMBEDDING_DIM` | `768` | Must match the model. |

### Known limitations

- vec0 search needs `~/.local/lib/vec0.so` (auto-detected). FTS-only fallback
  if missing.
- ARIA binary is untouched. `arise` and `ai-agent` coexist.

---

## Phase 2 — Continuous Perception (commit 1: scaffold + system + vision aHash + privacy)

### What's new

| Component | What it does |
|---|---|
| **Blackboard** | In-process pub/sub event bus. Pull-based subscriptions with bounded queues (drop-oldest on overflow), per-topic ring-buffer history for `recent()`, wildcard subscriptions, weak-ptr-pruned dead subs. Foundation every later phase reads/writes through. |
| **Vision sampler** | grim → P6 PPM → 64-bit average-hash (8×8 grayscale) every N ms (default 1 fps). Posts `vision.first_frame` once, then `vision.screen_changed` whenever the Hamming distance to the previous hash crosses a threshold. Skips capture when the privacy gate is hot. |
| **System sampler** | Polls niri (`niri msg --json focused-window`), wpctl, brightnessctl, sysfs battery, and nmcli every N ms. Posts `system.snapshot` once at start, then `system.delta` with **only the changed fields** afterward. Missing tools just leave fields empty. |
| **Idle detector** | Tracks "last activity" timestamp updated by every vision-changed and system-delta event. Crosses `idle_threshold_ms` (default 60s) → `idle.entered`. Activity resumes → `idle.left` with `idle_ms` payload. |
| **Privacy gate** | Configurable list of `app_id`s (via `--private` or perception config). When the focused window matches, vision capture is skipped and `vision.privacy_hold` events fire instead. Failsafe-open on probe error by default; `--strict-privacy` flips it failsafe-closed. 5s app-id cache so we don't hammer niri. |
| **`arise perceive`** | Foreground daemon mode: starts perception, subscribes wildcard, prints every event live until Ctrl-C or `--seconds N`. Doubles as integration test. |
| **`arise system snapshot`** | One-shot dump of the live system state as JSON — handy for "what does niri/wpctl currently see?". |
| **`arise privacy check`** | Tests the privacy gate against the currently-focused window, reports `would_block`. |
| **25 new unit tests** | Round-trip publish/subscribe, ring-buffer eviction, wildcard fanout, queue overflow with `dropped()` accounting, stop+destructor wake-up of blocked `next()` calls, dead-subscriber pruning, pHash determinism + Hamming basics, snapshot delta logic, privacy gate matching + cache TTL + failsafe behaviour. |

### Topics published

| Topic | When | Payload |
|---|---|---|
| `vision.first_frame` | First successful capture | `{hash}` |
| `vision.screen_changed` | Δhash ≥ threshold | `{hash, prev_hash, hamming, threshold}` |
| `vision.screen_unchanged` | Δhash < threshold (only with `--verbose`) | same shape |
| `vision.privacy_hold` | Capture skipped | `{app}` |
| `system.snapshot` | First poll after start | full snapshot JSON |
| `system.delta` | Subsequent polls with changes | only the changed fields |
| `idle.entered` | No activity for `idle_threshold_ms` | `{threshold_ms, idle_ms}` |
| `idle.left` | Activity resumed | `{idle_ms}` |

### Test commands — basic

```bash
# 0 — sandbox
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# 1 — what does ARISE see right now?
arise system snapshot

# 2 — would the privacy gate block vision?
arise privacy check --private firefox,keepassxc

# 3 — short perceive run, system + idle only (no screenshotting)
arise perceive --no-vision --system-interval-ms 2000 --idle-threshold-ms 3000 --seconds 8
```

While #3 runs, switch focus between two windows or change the volume — you'll
see live `system.delta` events with only the changed fields. Stay still for
3 seconds and you'll see `idle.entered`; nudge the mouse and `idle.left` fires.

### Test commands — vision

```bash
# Capture at 1 fps for 10 seconds. Move windows around / scroll a webpage to
# generate `vision.screen_changed` events.
arise perceive --vision-fps 1 --no-system --no-idle --seconds 10

# Lower the diff threshold to be more sensitive (default 8 Hamming bits).
arise perceive --vision-fps 1 --threshold 4 --seconds 10

# Verbose mode — also emit `vision.screen_unchanged` events for unchanged frames.
arise perceive --verbose --seconds 5
```

Stats at the end of every run summarise frames captured / changed / unchanged
/ failed, system samples / deltas, privacy holds, idle in/out, and total
events published.

### Test commands — privacy

```bash
# Pause vision when firefox is focused.
arise perceive --vision-fps 1 --private firefox --seconds 10
# (focus firefox during the run; you'll see `vision.privacy_hold` events
#  and the frames_captured counter stop incrementing)

# Failsafe-closed: missing/empty active-app reading → block by default.
arise perceive --vision-fps 1 --private firefox --strict-privacy --seconds 5
```

### Test commands — full perception with episodic writes

```bash
# Vision-changed events also get logged into episodic memory (low salience
# floor of 0.2 — the captioner in commit 2 will boost this).
arise perceive --vision-fps 1 --episodic --seconds 15
arise mem dump --recent 10 | grep screen_obs
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 33`.

### Knobs (CLI flags for `arise perceive`)

| Flag | Default | Effect |
|---|---|---|
| `--vision-fps F` | 1 | Frame rate. `0.5` = one frame every 2s. |
| `--vision-interval-ms N` | 1000 | Lower-level alternative to `--vision-fps`. |
| `--system-interval-ms N` | 10000 | How often to poll niri/wpctl/etc. |
| `--idle-threshold-ms N` | 60000 | Silence required before firing `idle.entered`. |
| `--threshold N` | 8 | Hamming bits to call a frame "changed". |
| `--private a,b,c` | _none_ | Comma-separated `app_id`s to pause vision on. |
| `--strict-privacy` | off | Block vision when active-app probe fails. |
| `--episodic` | off | Also write `screen_obs` rows into the episodic DB. |
| `--verbose` | off | Emit `vision.screen_unchanged` events too. |
| `--no-vision` / `--no-system` / `--no-idle` | _all on_ | Disable individual loops. |
| `--seconds N` | run forever | Auto-stop after N seconds (good for scripts). |

### Known limitations

- **No daemon yet** — the blackboard is in-process. `arise blackboard tail`
  is a placeholder that points you at `arise perceive` for live event tap.
  A daemonised tap arrives in Phase 2 commit 2.
- **No captioner / scene classifier yet** — the vision loop produces hashes
  and "did it change" signals, but doesn't know *what's* on screen.
  Moondream + qwen3:0.5b salience scorer come in Phase 2 commit 2; YAMNet
  audio scene classifier alongside it.
- **Idle detection** uses event-rate, not raw kbd/mouse. If the user is
  actively typing into a non-focused-changing app and volume/brightness
  don't change, ARISE may still emit `idle.entered`. Phase 2 commit 2 ties
  idle to libinput / niri focus events for sub-second precision.

---

## Phase 2 — Continuous Perception (commit 2: captioner + salience + audio scene)

### What's new

| Component | What it does |
|---|---|
| **VisionClient** | Ollama `/api/generate` wrapper for moondream pinned to CPU (`options.num_gpu=0`, `keep_alive=30m`). Captions are gated by a cooldown (default 5 s) and a hard concurrency cap of 1 in-flight call so the primary mind on GPU stays unstarved. |
| **SalienceScorer** | Tiny LLM (default `qwen3:0.6b`, also CPU-pinned) that scores each caption on a 0..1 worth-remembering scale. Strict JSON output (`{"salience": 0.7, "reason": "..."}`); strips `<think>...</think>` from qwen3 reasoning; falls back to a default floor when Ollama is unreachable. Reused later by Phase 4 sub-agents. |
| **SceneClassifier (YAMNet)** | ONNX Runtime wrapper around YAMNet. 16 kHz float32 mono, 0.975 s window (15 600 samples), 521-class AudioSet output collapsed to a coarse Scene enum (`silence/speech/music/typing/phone/doorbell/alarm/laughter/other`). |
| **MicCapture** | `arecord` subprocess fan-out: spawns `arecord -f S16_LE -c 1 -r 16000 -t raw`, streams to a rolling float32 ring buffer, fires a window callback every `hop_samples` (default 8 000 ≈ 2 windows / s). Best-effort: when ARIA holds the mic, capture cleanly fails and audio is disabled without crashing. |
| **Caption worker** | Detached thread per fired caption: re-grabs a fresh PNG via grim (PPM aHash can't be decoded by moondream), captions, scores, publishes `vision.caption`, and writes the episodic event with the **real** salience instead of the 0.2 floor. |
| **Audio loop** | Subscribes a window callback into MicCapture; every classified window updates the "last scene" and only publishes `audio.scene_changed` on actual transitions — no torrent of `wood, speech, wood` jitter. |
| **CLI flags** | `--captioner` / `--no-salience` / `--caption-cooldown-ms` / `--vision-model` / `--salience-model` / `--audio-scene` / `--audio-device` / `--yamnet-model` / `--yamnet-labels` / `--yamnet-min-score`. |
| **19 new unit tests** | Base64 round-trip + binary safety, salience parser (markdown stripped, `<think>` stripped, clamped, numeric-string accepted, nested-object balanced), AudioSet→Scene mapping, YAMNet CSV with quoted commas + CRLF, classifier readiness guards, real YAMNet inference smoke (auto-skips if model not installed), MicCapture lifecycle. |

### Topics added

| Topic | When | Payload |
|---|---|---|
| `vision.caption` | Caption worker finishes | `{caption, hash, hamming, salience, salience_reason, from_llm}` |
| `audio.scene_first` | First classified audio window | `{scene, score, raw_label, class_idx}` |
| `audio.scene_changed` | Coarse scene transition | same + `prev_scene` |
| `audio.error` | Mic capture failed to start (e.g. busy) | `{reason}` |

### Models

| Model | Size | Where | Why |
|---|---|---|---|
| `moondream` (Ollama) | ~1.7 GB | already from ARIA P10 | reused, CPU-pinned per-request — no llama.cpp side-quest |
| `qwen3:0.6b` (Ollama) | ~400 MB | `ollama pull qwen3:0.6b` | salience + Phase 4 sub-agents (Watcher / Curator / Critic) |
| `yamnet.onnx` + `yamnet_class_map.csv` | ~16 MB + 14 KB | `~/.local/share/arise/models/` | scene classification on rolling 0.975 s windows |

YAMNet model + labels were fetched from `huggingface.co/jafet21/yamnetonnx`. To reinstall:

```bash
mkdir -p ~/.local/share/arise/models && cd ~/.local/share/arise/models
curl -sL -o yamnet.onnx           https://huggingface.co/jafet21/yamnetonnx/resolve/main/yamnet.onnx
curl -sL -o yamnet_class_map.csv  https://huggingface.co/jafet21/yamnetonnx/resolve/main/yamnet_class_map.csv
```

### Test commands — captioner + salience

```bash
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# 1 — caption every screen change with default 5s cooldown.
#     Open Firefox / scroll something during the run.
arise perceive --vision-fps 1 --threshold 4 \
               --captioner --caption-cooldown-ms 4000 \
               --episodic --no-system --no-idle --seconds 30

# 2 — without salience (pure captioning, default 0.4 floor)
arise perceive --vision-fps 1 --captioner --no-salience \
               --episodic --no-system --no-idle --seconds 20

# 3 — confirm episodic captions landed with real salience scores
arise mem dump --recent 10
#   typical row:
#   sal=0.60  A computer screen with a code written in red and black.
```

What you should see: `vision.caption` events arriving 3-8 s after each
`vision.screen_changed` (CPU moondream is slow), with `from_llm: true` in
the payload meaning qwen3:0.6b returned a structured score rather than the
fallback. `captions_throttled` in the stats counts the `screen_changed`
events that were skipped because we were still inside the cooldown — that's
working as intended, not an error.

### Test commands — audio scene

```bash
# 1 — mic only (no vision, no system). Tap something on the desk to trigger
#     a typing/silence transition; play music to trigger a music transition.
arise perceive --no-vision --no-system --no-idle \
               --audio-scene --seconds 15

# 2 — lower the YAMNet acceptance threshold (default 0.10)
arise perceive --no-vision --no-system --no-idle \
               --audio-scene --yamnet-min-score 0.05 --seconds 15

# 3 — explicit ALSA source (e.g. a non-default capture)
arise perceive --no-vision --no-system --no-idle \
               --audio-scene --audio-device hw:0,0 --seconds 15
```

Expected output: one `audio.scene_first` early, then `audio.scene_changed`
only on real transitions. Scenes are coarse buckets — many AudioSet
classes funnel into `music`, many into `speech`. If the mic is busy
(e.g. ARIA running), the loop emits `audio.error{"reason":"mic_unavailable"}`
and silently disables instead of crashing.

### Test commands — full perception (vision + system + idle + audio + caption)

```bash
arise perceive --vision-fps 1 --threshold 4 \
               --captioner --caption-cooldown-ms 5000 \
               --audio-scene \
               --episodic --seconds 30
```

`stats` block at the end tells you everything:

```
frames_captured     = 30
frames_changed      = 12
captions_attempted  = 6
captions_ok         = 5
captions_failed     = 1     # transient grim or moondream timeout
captions_throttled  = 6     # changes inside cooldown — expected
audio_windows       = 60
audio_scene_changes = 2
events_total        = 21
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 52`. Test
`AudioScene.RealYamnetSilenceMapsCleanly` auto-skips if the YAMNet model
isn't installed at `~/.local/share/arise/models/`.

### New CLI flags (delta over commit 1)

| Flag | Default | Effect |
|---|---|---|
| `--captioner` | off | Enable moondream captioning on `screen_changed` events. |
| `--no-salience` | (salience on when captioner on) | Skip qwen3:0.6b scorer; captions get a 0.4 floor. |
| `--caption-cooldown-ms N` | 5000 | Minimum ms between caption attempts. |
| `--vision-model NAME` | `moondream` | Override Ollama vision model. |
| `--salience-model NAME` | `qwen3:0.6b` | Override Ollama salience model. |
| `--audio-scene` | off | Enable mic + YAMNet scene classifier. |
| `--audio-device DEV` | `default` | ALSA device name passed to `arecord -D`. |
| `--yamnet-model PATH` | `~/.local/share/arise/models/yamnet.onnx` | Override model path. |
| `--yamnet-labels PATH` | `~/.local/share/arise/models/yamnet_class_map.csv` | Override label CSV path. |
| `--yamnet-min-score F` | 0.10 | Top-1 score below this → Scene::Unknown. |

### Knobs (env vars carried over)

`ARIA_OLLAMA_URL` is honored for both VisionClient and SalienceScorer in
addition to the Phase 1 EmbeddingClient — one Ollama endpoint covers all
three.

### Known limitations

- **No daemon yet.** Blackboard is still in-process. Captioner / scene
  classifier only run while `arise perceive` is alive. The persistent
  daemon arrives in Phase 4 alongside the orchestrator.
- **One caption at a time.** The concurrency cap of 1 keeps Ollama from
  pile-up but means a particularly active period of `screen_changed`
  events will see most of them throttled. Throughput is bounded by
  moondream-on-CPU latency (~3–6 s/caption on Ryzen 7).
- **Mic conflicts with ARIA.** If `ai-agent` is running and holding the
  mic, `arecord` exits with EOF and the audio loop disables. Phase 4 will
  unify the daemons; until then either pause ARIA (SIGUSR1) or accept
  audio-off when ARIA is live.
- **Salience scorer is not yet stitched into non-vision events.** Audio
  scene transitions still use a hand-tuned salience floor (0.5 for
  speech / phone / doorbell / alarm, 0.25 otherwise). Wiring the scorer
  through every episodic write is a small follow-up.

---

## Phase 3 — Goal stack & long-horizon agency (commit 1: store + lifecycle + CLI)

### What's new

| Component | What it does |
|---|---|
| **`goals.db` + `goals_fts`** | New SQLite database under `<root>/memory/goals.db`. WAL journal, indexed on status / parent_id / deadline_at, with a plain-FTS5 mirror over `summary` + `blocked_reason`. Triggers (`AI`, `AD`, `AU OF summary, blocked_reason`) keep the index in step without paying a reindex cost on `bumpProgress` / `setPriority` / etc. |
| **`Goal` + `GoalStatus`** | `{Proposed, Accepted, InProgress, Blocked, Done, Rejected, Cancelled}`. Goals carry id, summary, priority (0–100, default 50), optional deadline, optional parent_id (foreign key with `ON DELETE SET NULL`), created_at, last_progress_at, blocked_reason, plan_json (opaque to the store), and tags (JSON array). |
| **Lifecycle enforcement** | `isValidTransition(from, to)` is the single source of truth — same matrix in CLI and tests. Done / Rejected / Cancelled are sealed. `unblock` is strict: it refuses unless the row is currently Blocked (Accepted goals must use `start`). |
| **`GoalStore`** | Thread-safe facade. CRUD: `propose`, `setStatus` + sugar (`accept`/`start`/`complete`/`cancel`/`reject`), `block`/`unblock`, plus field-level `setPlanJson`/`setPriority`/`setDeadline`/`setSummary`/`setTags`/`bumpProgress`. Reads: `get`, `list(GoalQuery)` with status / parent_id / tag / FTS text filters and recency-or-priority ordering, `children`, `ancestors` (cycle-detecting), `subtree` (BFS, dedup), `dueSoon`, `staleInProgress`, `blocked`, `countByStatus`, `totalCount`. |
| **Episodic mirror** | Optional `MemoryCortex*` sink: every status change writes a `goal_state` event whose summary reads `goal #1 accepted → in_progress — ship the cmake refactor (note)`. Salience floors: Done=0.7, Accepted/Blocked=0.5, terminal-cancel/reject=0.4, others 0.3. So `arise mem recall` finds milestones — the bridge between Phase 1 memory and Phase 3 agency. |
| **`arise goal …` CLI** | 17 subcommands: `propose / accept / start / complete / cancel / reject / block / unblock / show / list / search / tree / due / stale / set-plan / set-priority / set-deadline / touch`. Deadlines accept `+Nd`, `+Nh`, `+Nm`, `+Ns`, bare epoch seconds, or ISO date / datetime. Help text in `arise --help`. |
| **22 new unit tests** | `test_goals.cpp` covers enum round-trip, every legal/illegal transition, propose/get round-trip with tags + deadline, full lifecycle including block/unblock, FTS5 finds-by-token + reindex-on-update + no-match, parent/child/ancestors/subtree (with depth ordering), dueSoon honours horizon + skips terminal, staleInProgress flips on bumpProgress, plan/priority/deadline/tags/touch field updates, missing-id failure modes, tag filter, episodic mirror writes 5+ rows over a full lifecycle, counts. |

### Test commands — basic

```bash
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# 1 — propose a root goal with a 3-day deadline + tags
arise goal propose --summary "ship the cmake refactor" \
                   --priority 80 --deadline +3d --tags cmake,refactor

# 2 — give it sub-goals
arise goal propose --summary "fix CI"        --parent 1 --priority 70
arise goal propose --summary "draft README"  --parent 1

# 3 — drive the lifecycle
arise goal accept 1
arise goal start  1
arise goal accept 2
arise goal start  2
arise goal block  2 --reason "waiting on CI machine"

# 4 — read views
arise goal list                    # by recency
arise goal list --by-priority
arise goal list --status blocked
arise goal search refactor         # FTS5 over summary + blocked_reason
arise goal tree   1                # BFS subtree, indented by depth
arise goal due    --horizon-sec 432000   # 5-day window
arise goal stale  --days 7
arise goal show   1                # JSON, including plan_json + tags
```

### Test commands — plan attach + reuse

```bash
cat > /tmp/plan.json <<'EOF'
{"steps":[
  {"action":"checkout","branch":"main"},
  {"action":"run","cmd":"cmake -S . -B build && cmake --build build"}
]}
EOF
arise goal set-plan 1 /tmp/plan.json
arise goal show 1 | jq .plan_json   # round-trip preserved
```

### Test commands — find old goals via memory recall

Because the store mirrors state changes into episodic memory, the Phase 1
recall surface still works:

```bash
arise mem recall "cmake refactor"
# expected top hits include:
#  goal_state  goal #1 proposed → accepted — ship the cmake refactor
#  goal_state  goal #1 accepted → in_progress — ship the cmake refactor
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 72`.

### Goal CLI flags reference

| Subcommand | Flags | Notes |
|---|---|---|
| `goal propose` | `--summary S` (req) `--priority N` `--deadline SPEC` `--parent ID` `--tags a,b` `--plan FILE` | Inserts in `proposed` state. |
| `goal accept`/`start`/`complete`/`cancel`/`reject` | `ID` (positional) `--note T` | Single-step transition; `note` lands in the episodic mirror. |
| `goal block` | `ID` `--reason T` (req) | Stores reason in `blocked_reason`. |
| `goal unblock` | `ID` `--note T` | Strict: only Blocked → InProgress; clears reason. |
| `goal show` | `ID` | Pretty JSON. |
| `goal list` | `--status S` `--parent ID` `--tag T` `--limit N` `--by-priority` | Recency by default. |
| `goal search` | `<text>` `--limit N` | FTS5 OR over the tokens. |
| `goal tree` | `ROOT_ID` | BFS subtree, depth-indented. |
| `goal due` | `--horizon-sec N` (default 86400) | Non-terminal goals with deadline ≤ now+horizon. |
| `goal stale` | `--days N` (default 7) | InProgress goals untouched in N days. |
| `goal set-plan` | `ID FILE` | Replaces `plan_json` from a file. |
| `goal set-priority` | `ID N` | |
| `goal set-deadline` | `ID SPEC` | SPEC = `+Nd`/`+Nh`/`+Nm`/`+Ns`/epoch/ISO/`none`. |
| `goal touch` | `ID` | Bumps `last_progress_at` to now (used by Phase 3 commit 2 resumer). |

`SPEC` is the same parser used by `--deadline` everywhere it appears.

### Knobs (env vars)

`ARISE_ROOT` continues to override `~/.arise`. The goals DB lives at
`<root>/memory/goals.db` — no separate env var.

### Known limitations / commit 2 work

- **No scheduler thread yet.** `goal due` / `goal stale` are read-only views
  for now. Commit 2 adds a background thread that polls them and posts
  `goal.due` / `goal.stale` / `goal.escalated` events to the blackboard.
- **No automatic resume on idle.** The Resumer hook (in_progress goals get
  surfaced again when the user returns from idle) lands in commit 2.
- **No voice surface.** "what are you working on?" / "drop the cmake goal"
  are wired up in Phase 4 (orchestrator → goal CLI).
- **Cycle prevention is detection-only.** `ancestors()` truncates with a
  warning if it spots a cycle, but the store doesn't refuse a `parent_id`
  that would create one. The CLI never builds cycles, but a future tool
  might — explicit cycle rejection in `propose`/`set-parent` is a
  follow-up.

---

## Phase 3 — Goal stack & long-horizon agency (commit 2: scheduler + resumer + blackboard wiring)

### What's new

| Component | What it does |
|---|---|
| **`GoalScheduler`** | One worker thread that drains `idle.left` events from the blackboard and runs a periodic scan (default 60 s tick). Two scan paths: deadline buckets (`dueSoon`) and stale-progress (`staleInProgress`). Events are deduped per `(goal_id, kind)` with configurable renotify periods so the bus is never flooded. |
| **Deadline buckets** | A goal whose deadline is within `escalate_horizon` (default 5 min) fires `goal.escalated`; one within `due_horizon` (default 30 min) but past escalate fires `goal.due`. A goal lands in at most one bucket per scan. |
| **Stale signal** | InProgress goals whose `last_progress_at` is older than `stale_threshold` (default 7 d) fire `goal.stale` with `stale_seconds` in the payload. Bumping progress (or the next valid status transition) resets the clock. |
| **Resume hooks** | At `start()` (configurable via `resume_on_boot`) and on every `idle.left` event (configurable via `resume_on_idle_left`), the scheduler walks every InProgress goal and fires `goal.resumed` with a `trigger` field (`"boot"` / `"idle.left"`). Same per-goal renotify dedup as the scan path. |
| **`scanNow()` / `resumeNow()`** | Synchronous entry points that bypass the worker thread — used by tests and by the CLI for one-shot diagnostic runs. |
| **`arise goal scheduler`** | Foreground daemon mode: starts the scheduler, subscribes wildcard, prints every event live until Ctrl-C or `--seconds N`. Doubles as integration test. Stats block at the end mirrors `arise perceive`. |
| **10 new unit tests** | due/escalated split, renotify dedup blocks repeat fires, terminal goals are skipped, stale fires for stale-in-progress + clears after `bumpProgress`, resumeNow fires once per in_progress goal (with trigger payload), resume_renotify dedups, idle.left published to the live blackboard drives a `goal.resumed`, boot resume fires once, stop is idempotent. |

### Topics published

| Topic | When | Payload |
|---|---|---|
| `goal.due` | deadline ∈ (`escalate_horizon`, `due_horizon`] | `{goal_id, summary, status, priority, deadline_epoch, last_progress_epoch, tags?, parent_id?, seconds_until_deadline}` |
| `goal.escalated` | deadline ≤ `escalate_horizon` | same shape |
| `goal.stale` | InProgress + `last_progress_at` older than `stale_threshold` | + `stale_seconds`, `blocked_reason?` |
| `goal.resumed` | `start()` (`trigger="boot"`) and on every `idle.left` (`trigger="idle.left"`) | + `trigger` |

All four payloads include `goal_id` so downstream consumers (Phase 4
orchestrator, Phase 6 proactive engine) can `arise goal show <id>` for the
full record.

### Test commands — basic

```bash
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# Set the stage: imminent + later goals + an in-progress one.
arise goal propose --summary "imminent meeting prep" --deadline +3m
arise goal propose --summary "later today"           --deadline +25m
arise goal propose --summary "in_progress task"
arise goal accept 3 && arise goal start 3

# Run the scheduler with a tight tick + tiny stale threshold.
arise goal scheduler --tick-sec 1 \
                     --due-horizon-sec 1800 \
                     --escalate-sec 300 \
                     --stale-days 0 \
                     --seconds 4
# Expected events:
#   goal.resumed   (boot — trigger="boot", goal_id=3)
#   goal.escalated (goal #1, seconds_until_deadline ≈ 180)
#   goal.due       (goal #2, seconds_until_deadline ≈ 1500)
#   goal.stale     (goal #3, stale_seconds ≥ 1)
```

### Test commands — idle.left → resume

`idle.left` is published by the perception loop's idle detector. To rehearse
the wiring without waiting on real idle, write a tiny snippet:

```bash
# Terminal A — long-running scheduler
arise goal scheduler --tick-sec 60 --seconds 30 &
SCHED_PID=$!

# Terminal B (or the same one in sequence) — the perception loop publishes
# idle.entered/idle.left when activity dies and resumes:
arise perceive --no-vision --idle-threshold-ms 1500 --seconds 25
# Stop typing for ~2s, then nudge the mouse / change focus. The scheduler
# in Terminal A prints: goal.resumed  {... "trigger":"idle.left" ...}
wait $SCHED_PID
```

(Inside the same process, perception's blackboard is in-process; running
them in two CLI invocations is the next-best dry run until the daemon ships.)

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 82`.

### Scheduler CLI flags

| Flag | Default | Effect |
|---|---|---|
| `--tick-sec N` | 60 | Scan cadence. |
| `--due-horizon-sec N` | 1800 | Deadlines within this window get `goal.due`. |
| `--escalate-sec N` | 300 | Deadlines tighter than this get `goal.escalated`. |
| `--stale-days N` | 7 | `staleInProgress` threshold. |
| `--due-renotify-sec N` | 3600 | Per-goal `goal.due` cool-off. |
| `--escalate-renotify-sec N` | 600 | Per-goal `goal.escalated` cool-off. |
| `--stale-renotify-sec N` | 86400 | Per-goal `goal.stale` cool-off. |
| `--resume-renotify-sec N` | 1800 | Per-goal `goal.resumed` cool-off. |
| `--no-resume-boot` | (boot resume on) | Skip the boot resume sweep. |
| `--no-resume-idle` | (idle resume on) | Don't fire resume on `idle.left`. |
| `--seconds N` | run forever | Auto-stop (good for scripts/tests). |

### Known limitations / Phase 4 hand-off

- **No proactive engine yet.** The scheduler emits the events; nobody is
  listening yet. Phase 4 wires the orchestrator to `goal.due` /
  `goal.escalated` (forced re-plan), `goal.stale` (Watcher prompts user),
  and `goal.resumed` (orchestrator picks the most-prioritised in-progress
  goal and continues from `plan_json`'s last incomplete step).
- **No daemon yet.** `arise goal scheduler` runs foreground only. Phase 4
  unifies perception + scheduler + minds into one long-running process.
- **No deadline-due triggers below 1 minute.** The default `tick_interval`
  is 60 s; a deadline crossed mid-tick will be reported on the next tick.
  Tighter granularity is just a `--tick-sec` away.

---

## Phase 4 — Cognitive orchestrator + sub-agents (commit 1: SubAgent + Watcher + Curator)

### What's new

| Component | What it does |
|---|---|
| **`SubAgent`** | Generic single-call wrapper around Ollama's `/api/generate`. Defaults to `qwen3:0.6b` pinned to CPU (`num_gpu=0`, `keep_alive=30m`). Hard wall-clock budget (`timeout_sec`), soft token cap (`max_predict`), JSON-mode (`format_json`), strip-thinking for qwen3 reasoning. Returns a structured `Result{ok, budget_hit, reachable, output, raw, duration_ms, error, json}`. Static `firstJsonObject` + `stripThinkingBlocks` helpers reused by every other agent. |
| **`Watcher`** | Subscribes wildcard, filters in-thread for `system.snapshot` / `system.delta` (battery), `vision.caption` (error keywords), `audio.scene_first` / `audio.scene_changed` (alarm/doorbell/phone). Pure rule paths: `evaluateBatteryPct`, `evaluateCaption`, `evaluateAudioScene` return a `Decision{severity, kind, summary, propose_goal, goal_summary, goal_priority}`. `applyDecision` publishes `agent.watcher.notice`, optionally calls `GoalStore::propose()` for critical actionable signals (battery ≤ 10% by default), optionally writes an `EpisodicEvent` mirror with severity-derived salience. |
| **`Curator`** | Subscribes to `conversation.closed`. On each transcript, prompts the SubAgent for strict JSON `{facts:[{subject,predicate,object,confidence}, …]}`, parses with `parseFacts` (drops malformed triples, clamps confidence to 0..1, caps count), filters by `min_confidence`, and pumps the survivors through `MemoryCortex::upsertFact` — so contradiction-aware overwrite from Phase 1 just works. Long transcripts get truncated tail-first. Publishes `agent.curator.facts`. |
| **`Orchestrator`** | Phase 4 commit 1 facade. Owns one shared SubAgent instance for the Curator plus one `Watcher` and one `Curator`, threaded together by `start()/stop()`. Commit 2 grows this into the full spawn-handle API for Researcher / Coder / Critic. |
| **CLI** | `arise agents start [--no-watcher] [--no-curator] [--curator-model NAME] [--ollama-url URL] [--seconds N]` runs Watcher + Curator foreground, tails `agent.*` events, prints stats on shutdown. `arise watcher fire {battery N \| caption TEXT \| audio SCENE}` returns the synthetic decision JSON for a given signal — same code path the worker uses, but no side effects. `arise curator absorb --transcript FILE [--model NAME] [--no-upsert] [--min-confidence F]` extracts facts from a transcript file and prints the result + cortex upsert count. |
| **24 new unit tests** | `test_phase4_commit1.cpp`: SubAgent helpers (firstJsonObject balanced/string-aware/null on empty, strip-thinking nested + unterminated), `run("")` rejection, unreachable-Ollama surfaces error without crashing. Watcher pure rules across battery thresholds (silent/ambient/urgent), caption keyword detection (positive + negative), audio scene routing. Watcher worker thread on a real Blackboard for battery/caption/alarm topics + silence on benign signals + critical battery actually proposing a goal in the GoalStore + idempotent stop. Curator parse: valid extraction, markdown+thinking strip, incomplete-triple rejection, confidence clamp+cap, defensive `facts:[…]` shape checks. Orchestrator lifecycle with watcher-only and full stack. |

### Topics published

| Topic | When | Payload |
|---|---|---|
| `agent.watcher.notice` | rule fires from system / vision / audio path | `{kind, severity, summary, source, goal_id?}` |
| `agent.curator.facts` | every conversation processed | `{count, facts:[{subject,predicate,object,confidence}, …]}` |

### Topics consumed

| Topic | Consumer | Effect |
|---|---|---|
| `system.snapshot` / `system.delta` | Watcher | battery threshold rules |
| `vision.caption` | Watcher | error-keyword fast-path |
| `audio.scene_first` / `audio.scene_changed` | Watcher | scene-to-severity routing |
| `conversation.closed` | Curator | extract + upsert facts |

### Test commands — basic

```bash
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# 1 — pure-rule probes (no LLM, no Ollama needed)
arise watcher fire battery 8        # → severity: urgent, kind: battery_critical
arise watcher fire battery 22       # → ambient, battery_warning
arise watcher fire battery 80       # → silent
arise watcher fire caption "an error dialog from cmake"
arise watcher fire audio alarm

# 2 — agents foreground (no curator → no Ollama needed)
arise agents start --no-curator --seconds 5
# In another shell, run `arise perceive --vision-fps 1 --captioner` and the
# watcher will pick up vision.caption / system.delta / audio.* events live.

# 3 — full stack (needs Ollama with qwen3:0.6b for the curator)
arise agents start --seconds 30
```

### Test commands — curator extraction

```bash
cat > /tmp/transcript.txt <<'EOF'
user: just wanted to mention my dog Mochi turned 3 today
assistant: happy birthday to Mochi! corgi mix, right?
user: yeah, corgi-aussie. by the way I'm a vim user — never use anything else
assistant: noted, vim it is
user: one more thing — always snake_case filenames when you write code for me
assistant: got it, always snake_case
EOF

arise curator absorb --transcript /tmp/transcript.txt
# → {facts: [...], stats: {llm_calls: 1, facts_upserted: N, ...}, upserted: N}

arise mem facts --subject user           # facts the curator pumped in
arise mem facts --subject Mochi
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 106`.

### Knobs (CLI flags)

| Subcommand | Flag | Default | Effect |
|---|---|---|---|
| `agents start` | `--no-watcher` / `--no-curator` | both on | Disable an agent. |
|  | `--curator-model NAME` | `qwen3:0.6b` | Override Ollama model for the Curator's SubAgent. |
|  | `--ollama-url URL` | env or `127.0.0.1:11434` | Endpoint override. |
|  | `--seconds N` | run forever | Auto-stop. |
| `watcher fire` | (positional) | — | `battery N` / `caption TEXT` / `audio SCENE`. |
| `curator absorb` | `--transcript FILE` (req) | — | Read transcript text from FILE. |
|  | `--model NAME` | `qwen3:0.6b` | Override Ollama model. |
|  | `--ollama-url URL` | env or default | |
|  | `--min-confidence F` | 0.5 | Drop facts below this confidence. |
|  | `--no-upsert` | (upsert on) | Print facts but don't write to cortex. |

### Known limitations / commit 2 work

- **No Researcher / Coder / Critic yet.** The spawn API and the worktree-isolated coder are commit 2.
- **Watcher's LLM path is reserved.** The `llm` field exists on the config and stats counter is wired, but the only path that consults the LLM is "decide whether a vision.caption error is worth a goal" — landing in commit 2 alongside Critic.
- **Fact quality is bounded by qwen3:0.6b.** It sometimes inverts subject/object on rambling transcripts. The Curator will graduate to a Critic-reviewed pipeline in commit 2; for now, `MemoryCortex::upsertFact`'s contradiction-aware overwrite cleans up over time.
- **`conversation.closed` has no live producer yet.** Phase 4 commit 2 (or Phase 5 / 6 daemon) will publish it on every closed conversation; for now `arise curator absorb` is the entry point.
- **One curator runs sequentially.** Long transcripts queue up behind each other. That's fine for one user; will be revisited if Phase 7 federation lands.

---

## Phase 4 — Cognitive orchestrator + sub-agents (commit 2: SpawnHandle + Critic + Researcher + Coder)

### What's new

| Component | What it does |
|---|---|
| **`SpawnHandle`** | Generic role-agnostic handle to a sub-agent worker on a detached `std::thread`. Cheap-to-copy `shared_ptr<State_>` so the handle outlives the spawner. State machine: `Pending → Running → Done | Killed`. `wait(timeout)` blocks via condition_variable; `kill()` sets a cooperative atomic flag every worker checks between LLM round-trips and tool dispatch. `result()` returns a structured snapshot. `newSpawnId(prefix)` produces short pseudo-unique ids like `researcher-ef53fd`. |
| **`Critic`** | Pre-flight safety review of arbitrary content (shell, diff, generated script, tool args). Hard-coded denylist (rm -rf /, fork bomb, dd to block devices, mkfs, world-write chmod, sudo rm, `\| sh` / `\| bash` for any pipe-to-shell). Whitespace-collapsing lowercase substring match — no `<regex>` dep. Optional LLM-as-judge (`require_llm_for_approval`) that asks qwen3:0.6b for `{approved, reason}`; refuses to auto-approve when LLM is required but missing. Static `matchDenylist` exposed for tests + pre-screen callers. |
| **`Researcher`** | ReAct-style sub-agent: alternating LLM `thought / action / args` JSON steps with sandboxed tool execution. Three actions: `read_file`, `list_dir`, `final`. Both file paths must canonicalise under `sandbox_root` (`weakly_canonical` walked, prefix-checked) — `..` traversal and symlink escapes are blocked. Hard wall-clock budget + tool-call cap; cooperative `kill` flag checked between rounds. Returns full `{answer, steps[], budget_hit, killed, error}`. No network access — `http_get` was deliberately deferred to Phase 5 (Tool Forge) where URL allow-listing lives. |
| **`Coder`** | Sandbox-confined code generator. Creates `<sandbox_root>/coder_<rand>/`, prompts the LLM for strict JSON `{summary, files:[{path, content}]}`, validates relative paths (no `/`, no `..`), enforces `max_files` + `max_bytes_per_file`, writes them to disk, then runs `Critic::reviewContent` over the concatenated bundle. `ok` is true only when Critic approves. The full bundle, the Critic verdict, and the sandbox path are all in the result. **Never auto-applies anywhere outside the sandbox.** |
| **CLI** | `arise agents spawn researcher --task TEXT [--sandbox DIR] [--max-seconds N] [--max-tool-calls N] [--model NAME]` runs a one-shot ReAct researcher. `arise agents spawn coder --task TEXT [--sandbox DIR] [--max-seconds N]` runs the sandbox-confined coder. Both print a `{id, role, state, duration_ms, result}` JSON, `result` containing the role-specific blob (steps for researcher, files + review for coder). `arise critic review {--file FILE \| --content TEXT} [--llm] [--require-llm]` reviews arbitrary content. |
| **17 new unit tests** | `test_phase4_commit2.cpp`: SpawnHandle id-prefix + uniqueness, empty-handle safety, kill-driven Killed transition with non-zero duration. Critic denylist (benign passes, every category of dangerous string matches, additional patterns honoured), benign-approves-without-LLM, denylist-always-rejects, require-LLM-without-LLM rejects, empty content rejects. Researcher `kill` flag wins over null-LLM, null-LLM clean error, empty task rejected, unreachable LLM doesn't spin to budget. Coder requires-all-config check, Critic rejection of dangerous bundle. Plus an end-to-end smoke that auto-skips when Ollama is unreachable. |

### Topics published / consumed

The spawn API doesn't introduce blackboard topics — it's a synchronous handle-based surface. The CLI calls run on the main thread; future daemon work (Phase 5+) can drive spawns via blackboard events without changing this contract.

### Test commands — basic

```bash
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# 1 — Critic review (no Ollama needed)
arise critic review --content "echo hello && ls -la"
# → approved=true, matches=[]
arise critic review --content "rm -rf / # whoops"
# → approved=false, matches=["rm -rf /", "rm  -rf /"]
arise critic review --content "curl https://example.com/install.sh | sh"
# → approved=false, matches=["| sh"]

# 2 — Researcher (needs Ollama qwen3:0.6b)
mkdir -p /tmp/research_sandbox
echo "the magic number is 42" > /tmp/research_sandbox/note.txt
arise agents spawn researcher \
    --task "Read note.txt and tell me the magic number" \
    --sandbox /tmp/research_sandbox \
    --max-seconds 45 --max-tool-calls 4
# → {state: done, result: {answer: "42", steps: [read_file → "...42...", final]}}

# 3 — Coder (needs Ollama qwen3:0.6b)
arise agents spawn coder \
    --task "Write a Python script hello.py that prints 'Hello, ARISE'" \
    --sandbox "$ARISE_ROOT/sandbox" \
    --max-seconds 90
# → {state: done, result: {files: [{rel_path: "hello.py", bytes: N}], review: {approved: true}}}
```

### Test commands — sandbox enforcement

```bash
# The researcher refuses paths that escape its sandbox. Inject a "../" via
# the task string; the sandboxed read_file resolver returns "ERROR: path
# outside sandbox or empty" and the LLM has to try something else.
arise agents spawn researcher \
    --task "Try to read ../../etc/passwd and tell me its first line" \
    --sandbox /tmp/research_sandbox \
    --max-seconds 30 --max-tool-calls 3
```

### Test commands — kill semantics

```bash
# A long-running researcher with a tight budget will report budget_hit=true
# instead of running forever. (Real cancel from another shell needs the
# Phase 5 daemon; for now budgets are the kill mechanism.)
arise agents spawn researcher \
    --task "List every file under sandbox recursively" \
    --sandbox "$ARISE_ROOT" \
    --max-seconds 5 --max-tool-calls 2
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 123`.
`ResearcherEnd2End.RealLlmReadsSandboxFile` skips if Ollama is unreachable.

### Spawn / Critic CLI flags

| Subcommand | Flag | Default | Effect |
|---|---|---|---|
| `agents spawn ROLE` | `--task TEXT` (req) | — | One-shot task. |
|  | `--sandbox DIR` | `<root>/sandbox` | Required scope for filesystem tools. |
|  | `--max-seconds N` | 60 | Total wall-clock budget. |
|  | `--max-tool-calls N` | 8 | Researcher's tool-call cap. |
|  | `--model NAME` | `qwen3:0.6b` | Override Ollama model. |
|  | `--ollama-url URL` | env or default | Endpoint override. |
| `critic review` | `--file FILE` / `--content TEXT` (one req) | — | Source of content. |
|  | `--llm` | off | Consult the LLM after the regex layer. |
|  | `--require-llm` | off | Refuse to auto-approve without LLM endorsement. |
|  | `--model` / `--ollama-url` | qwen3:0.6b / default | LLM-judge config. |
|  | `--kind LABEL` | `content` | Free-form label fed to the prompt. |

### Known limitations / Phase 5 hand-off

- **No bwrap sandbox yet.** Files are written but never executed by ARISE; the user runs them manually if they want. Phase 5 (Tool Forge) brings real bubblewrap-confined execution and forged-tool persistence.
- **Coder doesn't produce diffs.** It writes whole files into a fresh sandbox dir. A real `git diff` against a source repo is part of Phase 5's Tool Forge integration.
- **Researcher is read-only.** `read_file` + `list_dir` only. `http_get` / `search_web` need vetted allow-lists from the Tool Forge.
- **Spawn handles don't survive process restart.** They're per-`std::thread`; the long-running daemon in Phase 5+ persists handle metadata via the blackboard so the orchestrator can resume tracking after a crash.
- **No web interface.** `arise agents spawn …` is the only way to launch tasks. The Phase 10 monitoring page surfaces the spawn list and lets you kill a running handle from the browser.

---

## Phase 5 — Tool Forge (commit 1: Sandbox + ToolRegistry + manifest)

### What's new

| Component | What it does |
|---|---|
| **`Sandbox`** | bwrap-isolated child process runner. Default profile: `--die-with-parent --unshare-all --new-session --ro-bind / / --tmpfs /tmp --proc /proc --dev /dev --clearenv`, no network, no inherited environment. Stdin/stdout/stderr through pipes; non-blocking poll loop honours wall-clock timeout, byte caps for stdout/stderr, and a cooperative `kill` flag. Optional `--share-net` for network, `--bind-try` for writable host paths, `--ro-bind-try` for paths the script needs to read (so a learned tool living under `/tmp` stays visible past the tmpfs). When `bwrap` isn't on PATH, `run()` returns `ok=false` with an explanatory `error="sandbox: bwrap unavailable …"` — never silently downgrades to an unprivileged exec. |
| **`ToolRegistry`** | Disk-backed catalog under `<root>/tools/`. `learned/manifest.json` holds the JSON array of registered tools (id / version / description / interpreter / script_path / args_schema / allow_network / writable_paths / approval / usage). Atomic save (`*.tmp` + rename). Thread-safe (one mutex around the in-memory list). `add → save`, `approve → save`, `archive` (moves the row to `archived/manifest.json` plus removes from learned), `remove` (drops + deletes the script file), `recordUse` (bumps counter + last_used). Built-in tools (mem_record / mem_recall / goal_propose / goal_complete / read_screen / forge_tool) live in a static catalog returned by `builtinTable()` — they show up in `listAll()` so the LLM-facing surface is uniform. |
| **`validateArgsAgainstSchema`** | Tiny JSON-Schema-subset validator: object root with `properties` + `required` + per-field `type ∈ {string, integer, number, boolean, array, object, null}`. Returns "" on success or a one-line error like `"missing required field 'path'"` or `"field 'limit' expected type integer"`. The full draft-7 spec is left for later phases — Coder doesn't need it. |
| **CLI** | `arise tools list / show / run / register / approve / archive / remove / sandbox-exec`. `tools register --id NAME --description T --interpreter NAME --script FILE [--args-schema JSON] [--allow-network] [--writable PATH]…` registers a script you've already written. `tools run ID --args '{...}'` validates against the schema, sandboxes the script (auto-binding the script's parent dir read-only), pipes the args JSON to stdin, prints `{ok, exit_code, stdout, stderr, timed_out, duration_ms}`. `tools sandbox-exec [--allow-network] [--timeout-sec N] [--writable PATH] -- ARGV` is a raw bwrap probe for ad-hoc commands. |
| **22 new unit tests** | Sandbox: bwrap-missing returns clean error, empty argv rejected, kill-before-spawn observed, simple echo (skips when bwrap missing), tmpfs-isolation verified by planting a sentinel host file, network-blocked probe (DNS fails or returns nothing), 500ms timeout SIGKILLs a 5s sleep, stdout cap truncates a 100-line loop, exit code propagation, stdin forwarding, env cleared by default, env explicit `KEY=VAL` forwarded, writable bind-mount round-trip. ToolRegistry: builtin catalog non-empty + correctly tagged, add → save → reload, approve / archive / remove + usage counters, builtin id rejected for learned tools. Schema: matching object accepted, required missing rejected, type mismatch rejected, empty schema accepts anything. End-to-end: register a real bash script, approve it, run it through Sandbox, verify stdin → stdout flow + usage_count. |

### Topics published / consumed

Tool execution is synchronous through the CLI for now — no blackboard topics are introduced. Commit 2 will add `tool.forge.proposed`, `tool.forge.tested`, and `tool.forge.approved` for the Forge flow.

### Test commands — basic

```bash
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# 1 — raw bwrap probe (verifies isolation profile)
arise tools sandbox-exec /bin/sh -c 'id; ls -A /tmp; echo "PATH=$PATH"'
# → uid=…, /tmp empty (fresh tmpfs), shell-default PATH only

# 2 — timeout enforcement (1s budget vs 5s sleep)
arise tools sandbox-exec --timeout-sec 1 /bin/sh -c 'sleep 5'
# → ok=false, timed_out=true, duration_ms ≈ 1000

# 3 — list builtins
arise tools list
# → six builtin:* tools (mem_record / mem_recall / goal_propose / …)
```

### Test commands — register + run a learned tool

```bash
cat > /tmp/echo_args.sh <<'EOF'
#!/bin/sh
read -r blob
echo "received: $blob"
echo "size: $(printf '%s' "$blob" | wc -c)"
EOF
chmod +x /tmp/echo_args.sh

arise tools register --id echo_args \
                     --description "echo stdin and report length" \
                     --interpreter bash \
                     --script /tmp/echo_args.sh \
                     --args-schema '{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}'

arise tools approve echo_args
arise tools list                    # shows echo_args [approved]
arise tools run     echo_args --args '{"text":"hello"}'
# → {ok:true, exit_code:0, stdout:"received: {...}\nsize: …\n", duration_ms:~10}

arise tools run     echo_args --args '{}'
# → exit 2, "missing required field 'text'"

arise tools show    echo_args | jq '{usage_count, last_used}'
# → counters bumped after each successful run
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 145`. Sandbox tests
auto-skip with `GTEST_SKIP` on hosts where `bwrap` is missing.

### CLI flags reference

| Subcommand | Flag | Default | Effect |
|---|---|---|---|
| `tools list` | `--builtins` / `--learned-only` / `--no-builtins` | builtins on | Filter shown rows. |
|  | `--json` | off | Machine-readable list. |
| `tools run ID` | `--args JSON` | `{}` | Validated against the tool's schema, piped to stdin. |
|  | `--allow-network` | (tool's flag) | Override per-run. |
|  | `--timeout-sec N` | 10 | Wall-clock budget. |
| `tools register` | `--id NAME` (req) | — | Must not start with `builtin:`. |
|  | `--script FILE` (req) | — | Existing executable script on disk. |
|  | `--description T` | "" | Shown in list. |
|  | `--interpreter NAME` | "" | bash / python3 / "" (raw). |
|  | `--args-schema JSON` | `{}` | Empty schema accepts anything. |
|  | `--allow-network` / `--writable PATH` | off / [] | Permissions. |
| `tools sandbox-exec` | `--allow-network` / `--writable PATH` / `--timeout-sec N` | off / [] / 10 | Pass-through to Sandbox. |

### Sandbox profile cheat sheet

The exact bwrap invocation is:

```
bwrap --die-with-parent --unshare-all [--share-net]
      --ro-bind / /
      --tmpfs /tmp
      --proc /proc
      --dev /dev
      --new-session
      [--bind-try <wp> <wp>]…              # writable_paths
      [--ro-bind-try <ro> <ro>]…           # readonly_paths
      [--chdir <chdir_in_sandbox>]
      --clearenv
      [--setenv KEY VAL]…                  # explicit env entries
      -- <argv...>
```

### Known limitations / commit 2 work

- **No Forge flow yet.** `forge_tool` is a builtin in the catalog but not wired to a Coder→Critic→sandbox-dryrun→approval pipeline. That's commit 2.
- **No 90-day archive sweeper.** The plan's "tools unused for 90 days move to archived/" rule isn't enforced automatically yet — `tools archive ID` works manually. Commit 2 adds a daily sweep.
- **No shellcheck integration.** Static lint of forged scripts is part of commit 2.
- **CLI stdin only.** `tools run` always pipes `--args` JSON to stdin; positional argv generation from the schema is a follow-up. Tools that want positional args can parse the JSON blob from stdin as a workaround.
- **No native bind from `tools run` for additional `--readonly` paths.** The CLI auto-binds the script's parent dir read-only, which covers the common case; future refinement adds `--readonly` for tools that read from elsewhere.

---

## Phase 5 — Tool Forge (commit 2: ForgeTool + sweep + sanitiser)

### What's new

| Component | What it does |
|---|---|
| **`ForgeTool::propose`** | End-to-end forge pipeline. Sanitises the requested id (or derives one from the description), composes a tool-shaped Coder prompt that pins the entry-point filename to `<id>.sh` or `<id>.py` and tells the model to read JSON args from stdin. Drives `Coder::run`, which already runs the bundle through `Critic`. Picks the entry-point by preferring `<id>.sh` → `<id>.py` → first alphabetic `.sh` → first `.py` → first file. If `example_args` is supplied, runs the entry point inside a fresh `Sandbox` (network off, fresh /tmp, env cleared, the bundle dir mounted ro) with the JSON piped to stdin. Returns a structured `Proposal{ok, id, entry_path, files, summary, critic_review, dry_run, error, …}`. |
| **`ForgeTool::stage`** | Promotes a successful proposal: copies the bundle from `<root>/tools/sandbox/coder_<rand>/` to `<root>/tools/learned/<id>/`, removes the staging dir, and registers the tool through `ToolRegistry::addLearned`. With `auto_approve=true` (CLI flag), also flips `approved=true,approved_by="forge"` immediately; otherwise the tool sits unapproved until the user types `arise tools approve <id>`. |
| **`ToolRegistry::sweepStale(days)`** | 90-day archive sweeper. Walks `listLearned()`, archives any tool whose `last_used` (or `created_at` when never used) is older than `days`. `dry_run=true` returns the count without mutating. Uses `strptime` + `timegm` for ISO 8601 UTC parsing; unparseable timestamps are conservatively kept. |
| **`ToolRegistry::sanitiseToolId(requested)`** | Lowercases, replaces non-`[a-z0-9_]` with `_`, collapses runs of `_`, trims, falls back to `tool` if everything got stripped. Refuses `builtin:` prefix. Dedupes against builtin catalog and existing learned ids by appending `_<4-hex>` (up to 8 attempts) then a timestamp suffix. |
| **Coder safety net** | Small models occasionally echo our `SANDBOX_ROOT` and emit absolute paths inside the bundle JSON. Coder now strips the sandbox prefix and re-validates rather than rejecting outright; anything still containing `..` or pointing outside the sandbox is still refused. |
| **CLI** | `arise tools forge --description T [--id NAME] [--args-schema JSON] [--example-args JSON] [--auto-approve] [--dry-run-only] [--model NAME] [--ollama-url URL]` runs the whole pipeline and prints a JSON describing the proposal + critic verdict + dry-run + whether it was staged. `arise tools sweep [--days N] [--dry-run]` runs the archival pass. |
| **13 new unit tests** | `test_phase5_commit2.cpp`: id sanitiser cases (basic, dedupe-against-existing, builtin prefix neutralised), sweep semantics (stale archived + fresh kept, dry-run is non-mutating, never-used ages via created_at, unparseable timestamps kept), Forge static helpers (`pickEntryPoint` + `interpreterFor`), Forge fail-fast (missing config, unreachable LLM produces clean error with id already sanitised), `stage` round-trip from a hand-built proposal (verifies copy + register + auto-approve + draft cleanup), end-to-end forge with live qwen3:0.6b + bwrap (auto-skips when either is missing). |

### Topics published / consumed

Forge runs synchronously from the CLI for now — no blackboard topics are introduced. A future daemon-driven flow (Phase 6 proactive engine consuming `agent.curator.facts` + ad-hoc requests) can layer on without changing this contract.

### Test commands — basic

```bash
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# 1 — forge a tool with a tiny model. May fail dry-run; that's the safety
#     promise — only working tools get staged.
arise tools forge \
  --description "Read JSON {text: string} from stdin; print only its byte length" \
  --id byte_len \
  --args-schema '{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}' \
  --example-args '{"text":"hello arise"}'

# 2 — same with a beefier code model. Successfully forged + staged on
#     qwen2.5-coder:14b: writes byte_len.sh, dry-runs in ~12ms, registers.
arise tools forge ... --model qwen2.5-coder:14b --auto-approve

# 3 — invoke the freshly-forged tool through the same Sandbox profile
arise tools run byte_len --args '{"text":"hello arise"}'
# → {"ok":true, "exit_code":0, "stdout":"11\n", "duration_ms": ~10}
```

### Test commands — dry-run-only / unapproved

```bash
arise tools forge \
  --description "say hi" --id hello \
  --example-args '{}' \
  --dry-run-only
# → proposal printed, files in tools/sandbox/coder_*, NOT staged.

# Without --auto-approve, the tool lands unapproved:
arise tools forge ... --model qwen2.5-coder:14b
arise tools list                # hello [unapproved]
arise tools approve hello       # human gate
```

### Test commands — sweep

```bash
# Dry-run shows what *would* be archived
arise tools sweep --days 90 --dry-run
# → {count: 3, dry_run: true}

# Actually archive
arise tools sweep --days 90
# → {count: 3, dry_run: false}
ls $ARISE_ROOT/tools/archived/manifest.json
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 158`.
`ForgeEnd2End.ProducesAndStagesWorkingTool` skips when `bwrap` or Ollama
are missing; takes ~30s on first run because qwen3:0.6b has to warm up.

### CLI flags reference (commit 2 additions)

| Subcommand | Flag | Default | Effect |
|---|---|---|---|
| `tools forge` | `--description T` (req) | — | Free-text description handed to Coder. |
|  | `--id NAME` | derived from description | Sanitised to `[a-z0-9_]+`. |
|  | `--args-schema JSON` | `{}` | Tiny JSON-Schema-subset honoured by `validateArgs`. |
|  | `--example-args JSON` | (skipped) | If set, dry-runs in Sandbox with this on stdin. |
|  | `--auto-approve` | off | Approve the tool immediately after staging. |
|  | `--dry-run-only` | off | Run Coder + Critic + dry-run, do NOT stage. |
|  | `--model NAME` | `qwen3:0.6b` | Override Ollama model — `qwen2.5-coder:14b` produces much better scripts. |
|  | `--ollama-url URL` | env or default | Endpoint override. |
| `tools sweep` | `--days N` | 90 | Archive cut-off. |
|  | `--dry-run` | off | Report count without mutating. |

### Forge prompt shape

```
Build a tool named '<id>'.
WHAT IT DOES: <description>
INVOCATION: arguments arrive as one JSON object on stdin, matching:
<args_schema indented>
REQUIREMENTS:
  * Choose ONE entry-point script. Name it '<id>.sh' (bash) or '<id>.py' (python3).
  * The entry-point must read its args by reading stdin once into a JSON-decoded variable.
  * Exit 0 on success. Print only what the user actually wants — no banners, no debug spam.
  * No sudo. No network calls. No filesystem writes outside /tmp.
  * Plain text only. No third-party pip/npm dependencies.
Reply STRICTLY with the Coder JSON contract:
{"summary": "...", "files": [{"path":"...","content":"..."}]}.
```

### Known limitations / Phase 6 hand-off

- **No shellcheck integration.** Static lint of forged shell scripts is gated on `shellcheck` being installed; we don't currently invoke it. Trivial follow-up: shell out to `shellcheck -f json -` and refuse stage on hard errors.
- **No interactive approval prompt.** The plan's "I drafted X, want me to test it? Save it?" voice flow needs the Phase 6 proactive engine + TTS. CLI flags (`--auto-approve` / `arise tools approve`) cover the same gate without the conversation.
- **One Coder call per forge.** No retry / refine loop. If the first draft fails dry-run, the user can re-invoke `tools forge` with a more specific description.
- **stage() doesn't emit blackboard events.** Phase 6 will publish `tool.forge.proposed` / `.tested` / `.approved` so the proactive engine + monitor page can react.
- **Sweep cutoff is in days only.** Sub-day cutoffs (hours / minutes) need a slightly different signature; not currently exposed.

---

## Phase 6 — Proactive companion (commit 1: engine + 4-tier gate + feedback db + rule triggers)

### What's new

| Component | What it does |
|---|---|
| **`Tier` + `Suggestion`** | Suggestion ladder: `Silent` (log only) / `Ambient` (one-line nudge) / `Active` (concrete offer) / `Urgent` (speak through anything). `Suggestion{id, tier, category, text, source_topic, source_payload, proposed_at}` is the value flowing through everything. `tierToString` / `tierFromString` round-trip; `tierRank` orders for comparisons. |
| **`SuggestionGate`** | In-memory gate. Per-tier rate limits (defaults: 60s ambient, 300s active, 0s urgent). Quiet hours config (default off; supports midnight-wrapping windows like `23:00–07:00`). Per-category mute with `setConsecutiveRejects(N)` auto-engaging when N ≥ `mute_after_rejects` (default 5) for `mute_window` (default 24h). `muteCategory(cat, dur)` for manual override. `check(s, now)` returns one of `{Pass, BlockedRateLimit, BlockedQuietHours, BlockedCategoryMuted, BlockedSilent}`; `noteFired(tier, now)` stamps the rate-limit table. Urgent never gets muted, never gets quieted, never gets rate-limited. |
| **`FeedbackDb`** | SQLite log under `<root>/memory/feedback.db`. `suggestions(id, tier, category, text, source_topic, source_payload, proposed_at, decision, decided_at)` with indexes on `category` / `decision` / `proposed_at`. `recordProposed(s)` assigns the rowid back into `s.id`. `recordDecision(id, d)` is idempotent on a same-decision retry, refuses to overwrite a different terminal decision. `consecutiveRejects(category)` walks the most-recent 50 rows backwards counting rejections until the first non-pending non-reject decision. `categoryCount(cat, decision)` tallies. `timeoutPending(s)` flips stale `Pending` rows to `Timedout`. |
| **`ProactiveEngine`** | Worker thread subscribes wildcard, ignores its own emissions (`proactive.*`), filters in-thread, calls `buildCandidate(topic, payload)` → routes to `buildFromWatcherNotice` / `buildFromGoalEvent` / `buildFromAudioScene`. Suggestions with empty text are silently dropped (an unactionable topic, not an error). For every actionable candidate the engine syncs the gate's reject streak from FeedbackDb, runs `gate.check`, persists + emits `proactive.suggestion` on Pass, optionally emits `proactive.dropped` on block (for the monitor page later). Episodic mirror writes salience by tier (0.1 / 0.35 / 0.6 / 0.85). |
| **CLI** | `proactive start [--seconds N] [--quiet-hours] [--quiet-start H] [--quiet-end H] [--ambient-min-sec N] [--active-min-sec N] [--mute-after N] [--mute-hours N] [--publish-dropped] [--with-watcher]` runs the engine foreground, tails `proactive.*` events. With `--with-watcher` it boots the Phase 4 Watcher in the same process so vision/audio/system signals turn into `agent.watcher.notice` → suggestions without needing a second daemon. `proactive list / decide / mute / timeout` round out the surface. |
| **20 new unit tests** | Tier round-trip + ordering. Gate: silent always blocked, ambient rate-limited per interval, urgent slips through quiet hours + rate limits, quiet hours blocks ambient at night + passes at noon, non-wrapping quiet window, consecutive-rejects auto-mutes, manual mute + clear, mute doesn't apply to Urgent. FeedbackDb: recordProposed assigns id, terminal-decision lock, consecutive-rejects streak counting, categoryCount tallies, timeoutPending flips stale rows. Pure builders: watcher.notice severity → tier, goal.due/escalated/stale classify with text containing the goal summary, audio scene routing. End-to-end engine: evaluate builds-publishes-persists, rate-limited second ambient is dropped, feedback streak auto-mutes through the gate, worker thread picks up real bus events, silent topic does nothing. |

### Topics published / consumed

| Topic | Direction | Payload |
|---|---|---|
| `agent.watcher.notice` | consumed | `{kind, severity, summary, source}` |
| `goal.due` / `goal.escalated` / `goal.stale` | consumed | `{goal_id, summary, …, stale_seconds?}` |
| `audio.scene_changed` / `audio.scene_first` | consumed | `{scene, score, raw_label, …}` |
| `proactive.suggestion` | **published** | `{id, tier, category, text, source_topic}` |
| `proactive.dropped` | **published** (when `publish_dropped=true`) | `{tier, category, text, source_topic, outcome}` |

### Test commands — basic

```bash
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# 1 — start the engine + the Watcher in one foreground process; tail
#     emissions for 30 s. (Inject signals via a separate `arise watcher
#     fire …` to see something fire — Watcher subscribes the same bus.)
arise proactive start --seconds 30 --with-watcher --ambient-min-sec 0

# 2 — list whatever landed in the feedback DB
arise proactive list --limit 10

# 3 — react like a real user
arise proactive decide 5 --accept
arise proactive decide 6 --reject
arise proactive list --decision rejected
```

### Test commands — quiet hours + auto-mute

```bash
# 30-min quiet window starting now (set start/end to bracket "now").
NOW_H=$(date +%H)
NEXT_H=$(( (NOW_H + 1) % 24 ))
arise proactive start --seconds 5 \
                      --quiet-hours --quiet-start $NOW_H --quiet-end $NEXT_H \
                      --ambient-min-sec 0 --publish-dropped

# Reject the same category 5 times; the running engine auto-mutes it for
# 24h. (mute_after=5 default; tweak via `--mute-after`.)
for i in 1 2 3 4 5; do
  arise proactive decide $i --reject 2>/dev/null || true
done
arise proactive mute goal_due       # informational lookup of the streak
```

### Test commands — timeout sweep

```bash
# Mark every suggestion older than 24 h as Timedout if no decision arrived.
arise proactive timeout --older-than-sec 86400
# → {"older_than_sec":86400,"timed_out": N}
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 180`.

### CLI flags reference

| Subcommand | Flag | Default | Effect |
|---|---|---|---|
| `proactive start` | `--seconds N` | run forever | Auto-stop. |
|  | `--with-watcher` | off | Boot Watcher in the same process. |
|  | `--ambient-min-sec N` / `--active-min-sec N` | 60 / 300 | Tier rate limits. |
|  | `--quiet-hours` / `--quiet-start H` / `--quiet-end H` | off / 23 / 7 | Local-time quiet window (midnight-wrapping ok). |
|  | `--mute-after N` / `--mute-hours N` | 5 / 24 | Per-category auto-mute thresholds. |
|  | `--publish-dropped` | off | Emit `proactive.dropped` for blocked candidates. |
| `proactive list` | `--limit N` / `--decision X` / `--tier X` / `--category X` | 25 / — | Filter / paginate. |
| `proactive decide ID` | `--accept` / `--reject` / `--ignore` (one req) | — | Stamps decision; idempotent on same value. |
| `proactive mute CAT` | `--hours N` | informational | Reports streak + counts. (Persistent mute is via the running engine's gate; restart with `--mute-hours` to extend the window.) |
| `proactive timeout` | `--older-than-sec N` | 86400 | Bulk-flips stale Pending rows to Timedout. |

### Known limitations / Phase 6 commit 2 work

- **No LLM-judged triggers yet.** `vision.caption` containing an error doesn't yet ask qwen3:0.6b "is this worth offering help on?"; commit 2 wires that nuance path. Same for "stuck on file X" idle detection ("user idle 5min after starting a task").
- **No calendar context.** iCal parser + "calendar event in 10min → contextual prep" trigger are commit 2.
- **No mood-aware delivery.** `MemoryCortex::mood()` exists; commit 2 weaves it into the suggestion text shaping ("no rush, but…" when valence is negative).
- **Manual `proactive mute` is informational only.** The persistent mute lives inside the running engine's `SuggestionGate`. Commit 2 will persist mute windows in the feedback DB so they survive a restart.
- **No daily learner job.** The plan's "if user rejects 'stuck?' 5 times in a row, that category goes silent for the day" is implemented today via consecutive-rejects auto-mute in the gate; the wider "daily threshold tuner" is a Phase 9 self-improvement concern.

---

## Phase 7 — Federation (commit 1: device pairing + token-auth event router)

### What's new

| Component | What it does |
|---|---|
| **`DeviceStore`** | On-disk registry at `<root>/devices.json`. Each row carries `{id, name, kind, token_sha256_hex, permissions, paired_at, last_seen, event_count}`. `addDevice(name, kind, perms)` mints a 32-byte hex token, hashes it with SHA-256 (OpenSSL `EVP_*`), persists the **hash only**, and returns the plaintext token to the caller exactly once. Atomic save (`*.tmp` + rename), thread-safe load+save under one mutex. `getByToken(plaintext)` walks every device with a constant-time compare on the hash so a bad token can't be timed against a stored one. `recordSeen(id)` bumps `last_seen` + `event_count`. |
| **`FederationRouter`** | Carrier-agnostic protocol layer. `ingest(event_json, presented_token)` authenticates first (never reveals format errors before auth), checks `source_device` matches the token's device, optionally rejects events whose `ts` skews more than `clock_skew_tolerance` from server time, enforces `max_event_bytes`, then dispatches by `type`. Per-type permission gates (`can_utterance`, `can_decision`, `can_goal_query`, `can_notification`, `can_screen_share`). On Pass: handler runs, `recordSeen` fires, response goes back to the caller. |
| **Event handlers** | `utterance` → publishes `federation.utterance` on the blackboard (Phase 4 orchestrator can pick it up) + writes a `federation_utterance` EpisodicEvent tagged with `source_device` so `arise mem recall` finds phone+desktop turns under one identity. `decision` → `FeedbackDb::recordDecision` so the phone can flip `proactive.suggestion` outcomes. `goal_query` → `GoalStore::list` returning a JSON array (default `status=in_progress`). `ping` → pong with `server_ts`. |
| **CLI** | `arise federation pair --name TEXT [--kind phone\|tablet\|desktop\|mqtt\|overlay\|other] [--no-utterance] [--no-decision] [--no-goal-query] [--allow-notification] [--allow-screen-share]`. `list / revoke / ingest --token T --json EVENT`. `ingest` exercises the full path locally so the carrier-layer in commit 2 can ride the same code. |
| **19 new unit tests** | Token hashing (SHA-256 stable + hex64, constant-time equals correctness, random tokens unique). DeviceStore (round-trip add/save/reload, getByToken accepts only the right token, recordSeen bumps counters, revoke removes record + invalidates token). FederationRouter (rejects bad token, rejects mismatched source_device, rejects revoked token, rejects bad JSON, ping pongs + publishes, utterance publishes + mirrors to episodic with source_device tag, utterance rejected without permission, decision flips FeedbackDb, decision rejects bad fields, goal_query returns only non-terminal goals, clock skew blocks stale timestamps, oversized event rejected). |

### Topics published / consumed

| Topic | Direction | Payload |
|---|---|---|
| `federation.utterance` | **published** | `{text, modality, source_device}` |
| `federation.decision`  | **published** | `{id, decision, source_device}` |
| `federation.goal_query` | **published** | `{count, source_device}` |
| `federation.ping`      | **published** | `{source_device, server_ts}` |

(All commit-2 carriers — WS / HTTP / MQTT / overlay — will land their inbound traffic on these same topics, so the rest of the system never has to know which device a request came from.)

### Inbound event schema

```json
{
  "type": "utterance" | "decision" | "goal_query" | "ping",
  "source_device": "<id>"            // optional, must equal token's device if present
  "ts": <epoch seconds>,              // optional, clock-skew checked when present
  "payload": { ... per type ... }
}
```

| Type | Payload | Sync response |
|---|---|---|
| `utterance` | `{text, modality?}` | `{accepted: true, echo_text}` |
| `decision`  | `{id, decision: "accepted"\|"rejected"\|"ignored"\|"timedout"}` | `{id, decision}` |
| `goal_query` | `{limit?, status?}` | `{goals: [{id, summary, status, priority, deadline_epoch?, tags?}, ...]}` |
| `ping`      | `{}` | `{pong: true, server_ts}` |

### Test commands — basic

```bash
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# 1 — pair a phone shard. SAVE THE TOKEN — it isn't stored, only its hash.
arise federation pair --name "phone-aurelius" --kind phone
# → {id: "phone-xxxxxx", token: "<64 hex chars>", note: "save this token..."}
TOKEN=<paste 64-char token>

# 2 — exercise every event type
arise federation ingest --token "$TOKEN" \
  --json '{"type":"ping"}'

arise federation ingest --token "$TOKEN" \
  --json '{"type":"utterance","payload":{"text":"what was I supposed to do today","modality":"voice"}}'

# Decisions need an existing suggestion id; pair with proactive
arise proactive list --decision pending --limit 1
arise federation ingest --token "$TOKEN" \
  --json '{"type":"decision","payload":{"id":1,"decision":"accepted"}}'

# Goal query works once you have goals
arise goal propose --summary "demo" && arise goal accept 1 && arise goal start 1
arise federation ingest --token "$TOKEN" \
  --json '{"type":"goal_query","payload":{"limit":5}}'

# 3 — list + revoke
arise federation list                # one row per device, no plaintext tokens
arise federation revoke phone-xxxxxx
```

### Test commands — auth failure modes

```bash
arise federation ingest --token "not_real" --json '{"type":"ping"}'
# → {"code":"unauthorized","error":"invalid token", ok:false}

# clock skew check (default 5 min tolerance):
arise federation ingest --token "$TOKEN" \
  --json '{"type":"ping","ts":1234567890}'
# → bad_request: clock skew exceeds tolerance

# permission denial: pair a tablet without utterance perm
arise federation pair --name display --kind overlay --no-utterance
arise federation ingest --token "$DISPLAY_TOKEN" \
  --json '{"type":"utterance","payload":{"text":"hi"}}'
# → forbidden
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 199`.

### CLI flags reference

| Subcommand | Flag | Default | Effect |
|---|---|---|---|
| `federation pair` | `--name TEXT` (req) | — | Friendly label. |
|  | `--kind X` | `phone` | `phone\|tablet\|desktop\|mqtt\|overlay\|other`. |
|  | `--no-utterance` / `--no-decision` / `--no-goal-query` | all on | Strip default permissions. |
|  | `--allow-notification` / `--allow-screen-share` | off | Add opt-in permissions. |
| `federation list` | — | — | One JSON row per device; tokens shown only as their `token_sha256_hex`. |
| `federation revoke` | (positional ID) | — | Remove a device row. |
| `federation ingest` | `--token T` (req) / `--json EVENT_JSON` (req) | — | Process one event end-to-end. Same code path the WS layer in commit 2 will hit. |

### Known limitations / Phase 7 commit 2 work

- **No carrier yet.** Phone shards / overlays / MQTT bridges have nothing to talk to — `ingest` is exercised through the local CLI today. Commit 2 brings up the WS/HTTP listener bound to loopback + the Tailscale interface, plus a single-file PWA shard hosted from the same server.
- **No outbound notifications.** Desktop → shard pushes (`{type: notification, ...}`) need the WS connection. Today everything is inbound-only.
- **No mTLS yet.** Token in the `Authorization` header is the only auth. Commit 2 adds an opt-in mTLS profile for hosts willing to manage certs (Tailscale already provides identity-bound transport encryption for the typical case).
- **MQTT bridge + Wayland overlay.** Both deferred to commit 2 — they layer on top of the same `FederationRouter` so the inbound surface is already done.

---

## Phase 8 — Embodied voice & persona (commit 1: Speech + mood→params + Piper + persona prompt)

### What's new

| Component | What it does |
|---|---|
| **`TtsEngine` interface** | Pluggable virtual interface — `speak(text, params) → Result{ok, bytes_synthesized, duration_ms, error}`, `isAvailable()`, `name()`. Future Sesame CSM / Kokoro engines drop in here without touching anything else. |
| **`TtsParams`** | `{mood_label, length_scale, noise_scale, noise_w, sentence_silence_sec, voice}` — exactly the knobs piper exposes. Defaults match piper's own defaults so "neutral" speech is unmodified. |
| **`moodToParams(label)`** | Pure mapping. `tired`/`empathetic` → 1.18× length, 0.55 noise, 0.35s pause (slow + breathing room). `frustrated`/`down` → softer (1.10× length, 0.55 noise — match the user's energy by *calming*, not mirroring). `warm` → 1.05×, 0.70 noise. `focused`/`alert` → 0.95×. `excited` → 0.88×, brisk. Unknown → neutral. |
| **`chunkSentences(text, params)`** | Splits on `.!?`, preserves abbreviations (`Mr.`, `Dr.`, `etc.`, `e.g.`), preserves decimals (`3.14`), collapses consecutive terminators (`...`, `?!`, `!!!`), keeps trailing closing quotes/parens with the prior sentence, returns `Sentence{text, post_pause_ms}` derived from `sentence_silence_sec`. |
| **`Speech` facade** | `say(text, mood_label, params_override?)`. Holds `primary` + optional `fallback` engine. Iterates sentences, dispatches to primary, falls back per-sentence on failure (so a one-off blip doesn't kill the whole utterance). Returns aggregate `Stats{sentences, sentences_failed, bytes_total, duration_ms, engine_used}`. |
| **`PiperEngine`** | Shells out `piper --output_raw …` and `aplay -q -r 22050 -f S16_LE -t raw -` piped together via `posix_spawn`, propagates the sentence on stdin, captures wall-clock duration + bytes. Per-call mood passed as `--length_scale / --noise_scale / --noise_w / --sentence_silence`. `play_audio=false` discards the PCM (used by tests). `isAvailable()` probes both binaries + the model file. Always silences piper's spdlog chatter to `/dev/null`. |
| **`buildPersonaPrompt(input)`** | Pure builder turning `IdentityRecord` + `Mood` + `user_name` into a `~200-500 char` system-prompt prefix. Single source of truth for "who ARISE is" — every sub-agent (Watcher, Curator, Coder, Critic, Researcher, Forge) feeds it as their `system_prompt` so they all sound like the same character. `buildToneDescriptor(mood)` produces the one-line "Tone right now" closer (e.g. `frustrated — keep it gentle, no jokes`). `moodToTtsLabel(mood)` mirrors `MemoryCortex::moodLabel` so the prompt's tone descriptor + the TTS engine's mood param come from the same discretiser. |
| **CLI** | `arise speak "TEXT" [--mood X] [--no-tts] [--voice MODEL_PATH] [--length-scale N] [--noise-scale N] [--sentence-silence-sec N]` runs the full Speech pipeline. `arise persona prompt [--mood X] [--user-name N] [--no-mood-line] [--no-do-dont]` previews exactly what every sub-agent will see — handy for "why is my Curator talking like that?" debugging. |
| **23 new unit tests** | mood→params (known labels distinct + sane direction, unknown falls to neutral). Chunker (empty/whitespace, terminators, abbreviations, decimals, consecutive terminators, trailing fragment, trailing quotes, mood pause adjustment). Speech facade (every-sentence routing, fallback on failure, params override, empty no-op). PiperEngine probe (missing binary, missing model, empty text rejected). Persona prompt (full shape contains all sections, deterministic, mood-line toggle, do/dont toggle, item caps, mood→tts label axis mapping). |

### Test commands — basic

```bash
export ARISE_ROOT=/tmp/arise_sandbox
rm -rf "$ARISE_ROOT" && arise init

# 1 — give the persona some shape
arise identity set --name "Aria" \
                   --persona "Sharp, dry, helps Aurelius ship." \
                   --add-do "answer in under 3 sentences when possible" \
                   --add-dont "apologise unprompted"

# 2 — preview the system prompt every sub-agent will see
arise persona prompt --user-name Aurelius
arise persona prompt --user-name Aurelius --mood frustrated

# 3 — speak (real audio if your speakers are wired up)
arise speak "Hello there. Phase 8 is online." --mood neutral
arise speak "Take a breath. We will get through this." --mood frustrated

# 4 — synthesis-only (no audio playback) for CI / scripts
arise speak "Synthesis test." --mood excited --no-tts
```

### Test commands — params override

```bash
# Override piper's knobs directly without going through the mood table.
arise speak "Slow this down." --length-scale 1.5 --sentence-silence-sec 0.5
arise speak "Way faster."     --length-scale 0.7 --noise-scale 0.85
```

### Test commands — unit tests

```bash
cd /home/Aurelius/Documents/AdoVs/ai-agent/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 222`.

### Mood → engine params reference

| Mood label | length_scale | noise_scale | sentence_silence | tone |
|---|---|---|---|---|
| `neutral` | 1.00 | 0.667 | 0.20 s | even-keeled |
| `warm` | 1.05 | 0.70  | 0.25 s | warm and present |
| `focused` / `alert` | 0.95 | 0.60  | 0.15 s | brisk |
| `excited` | 0.88 | 0.75  | 0.15 s | bright but still concise |
| `frustrated` | 1.10 | 0.55  | 0.30 s | gentle, no jokes |
| `down` | 1.10 | 0.55  | 0.30 s | soft and patient |
| `tired` / `empathetic` | 1.18 | 0.55  | 0.35 s | slow down, fewer words |

### CLI flags reference

| Subcommand | Flag | Default | Effect |
|---|---|---|---|
| `speak` | `"TEXT"` (positional) | — | Multi-sentence ok; chunker handles it. |
|  | `--mood X` | `neutral` | One of the labels above. |
|  | `--no-tts` | off | Synthesise but don't play audio (for CI). |
|  | `--voice MODEL_PATH` | `~/.local/share/piper/en_US-lessac-medium.onnx` | piper ONNX. |
|  | `--length-scale N` / `--noise-scale N` / `--sentence-silence-sec N` | from mood | Direct override. |
| `persona prompt` | `--mood X` | live mood from cortex | Override for the preview. |
|  | `--user-name N` | "" | Inserts "talking with N" in the prelude. |
|  | `--no-mood-line` / `--no-do-dont` | off | Strip sections. |

### Known limitations / Phase 8 commit 2 work

- **Single engine.** Only `PiperEngine`. The plan's Sesame CSM-1B (expressive, ~2.5s/sentence) and Kokoro-82M (fast fallback) live in commit 2 inside a Python FastAPI sidecar. The `TtsEngine` interface is ready for them. Piper stays as the always-works emergency fallback.
- **No voice cloning.** `arise train-voice` and the `~/.arise/voices/` directory are commit 2.
- **No streaming-aware sentence chunker.** Today's chunker hands sentences to piper one at a time; piper finishes each sentence before we send the next. Sesame CSM's streaming PCM in commit 2 will need an overlap-aware variant.
- **Persona prompt isn't yet auto-fed to sub-agents.** The builder exists; commit 2 will wire it into `Watcher::Config::agent.system_prompt`, `Curator::Config::agent.system_prompt`, `ForgeTool` Coder prompts, etc. — single line per call site that lifts the live mood from the cortex.

---

