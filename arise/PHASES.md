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

