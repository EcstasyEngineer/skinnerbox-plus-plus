# SkinnerBox++

An operant conditioning chamber for your editor.

SkinnerBox++ is a self-contained Notepad++ plugin (Windows x64) that measures
two things about your writing — **momentum** (are you typing?) and **content
entropy** (are you typing *writing*, not junk?) — and reinforces the state
where both hold. Rewards are **coins that spawn ahead of your caret** and are
collected by typing into them, platformer-style: yellow coins for keeping the
words coming, red coins for holding real flow. Optional hardware rewards
through [Intiface Central](https://intiface.com/central/) ride the same
policy. The box rewards the behavior it wants more of. You are the occupant.

Session logs are **metadata-only** — behavioral features and reward events,
never your document text (unless you explicitly arm debug telemetry).

## How it works

```
keystrokes ─→ momentum + content gate ─→ FSM ─→ VI reward policy ─→ outputs
```

The whole machine is a three-state FSM:

```
              activity                score ≥ enter AND gate ok
    ┌──────┐ ────────►  ┌────────┐ ─────────────────────────► ┌──────┐
    │ IDLE │            │ TYPING │                            │ FLOW │
    └──────┘ ◄────────  └────────┘ ◄───────────────────────── └──────┘
             idle > 30 s           score < exit OR gate fails
```

- **Momentum.** Net chars/min and burst persistence, smoothed (EWMA), with
  hysteresis on the FLOW boundary (enter 0.70 / exit 0.50). Deleting heavily
  and leaving the window drag it down; brief in-focus pauses (≤ 9 s) don't
  break a burst — reaching for a word is part of the hike.
- **Content gate.** A must-pass conjunction over the last 600 chars you
  *typed* (deletions don't rewrite history): character entropy ≥ 3.4 bits,
  repeated-token mass ≤ 0.55, "uuuh"-class filler ≤ 6%, plus tail checks that
  catch "duper duper duper" the moment it happens. The gate evaluates as a
  first-fail chain, and the failing facet is *named* in the status bar —
  `thin`, `repeats`, `flat`, `filler`, `mash`, `drone`, or `echo` — so a
  closed gate is never anonymous. No content evidence = gate fails closed.
  Momentum alone can never reach FLOW. The gate is **Latin/ASCII-oriented**
  (byte stream, English bigrams); non-ASCII sessions will get noisier facet
  scores.
- **Reward policy.** Two tiers. *Red* (quality): variable-interval,
  state-gated — hold FLOW ≥ 30 s, eligibility matures on an exponential
  hazard (mean 2 min), fires while you're actively typing, hard 40 s
  cooldown. VI + a state gate reinforces *staying in the state*, not
  performing for the dispenser. *Yellow* (regularity): every ~250 net typed
  chars while you're producing at all — volume is reinforced separately from
  quality.
- **Outputs.** The caret line warms toward gold as flow rises (tonic). A
  reward spawns a **coin ahead of your caret** — about 7 seconds of typing
  away at your current rate, just short of the word wrap — and typing into
  it collects it: pop, a reinforcing ding (the two-note coin for yellow, a
  warmer chime plus a floating affirmation for red), and the caret-line
  bloom. Uncollected coins expire worthless; stopping never pays. With
  Intiface enabled, red coins also play an enveloped vibration (client-side
  intensity cap, 30% of the device's range by default — Buttplug has no
  server-side cap, so the plugin owns the stop and every envelope ends in an
  explicit zero).

Collecting is typing, nothing else: only a keystroke-sized insert that
crosses the coin's position pays out, and only keystroke-sized inserts count
toward the yellow tier — pasting across a coin, or Ctrl+V farming, never
mints or collects anything.

## Configuration

Everything lives in `SkinnerBoxPP.ini` (Plugins → SkinnerBox++ → **Open
config**). Reward channels are independently toggleable:

| channel | INI key | default | what it does |
|---|---|---|---|
| Coins | `[coins] enabled` | **on** | spawn-ahead coin overlay + collect SFX, both tiers |
| Coin sound | `[coins] sound` | **on** | synth bells, or drop `coin_yellow.wav` / `coin_red.wav` into `…\plugins\config\SkinnerBoxPP-sounds\` (`.ogg` bell? convert once: `ffmpeg -i bell.ogg coin_red.wav`) |
| Random rewards | `[policy] vi_reward` | **on** | the red tier's VI schedule |
| Intiface | `[intiface] enabled` | on | enveloped vibration on red coins (needs Intiface Central running; harmless if not) |
| Flow vibe | `[intiface] flow_vibe` | off | continuous tonic vibe (at `flow_vibe_level` × cap) whenever you're in FLOW |

Two **felt dials** compile onto the raw policy knobs, so you can tune by
feel instead of by hazard math:

- `[policy] generosity` — expected red rewards per 10 flow-minutes
  (canonical when set; compiles onto `mean_reward_interval_s`).
- `[intiface] strength` — 0–1 fraction of the device's range (compiles onto
  the intensity cap).

Affirmations for red-coin collects are configurable:
`[coins] affirmations=good girl|good job|keep going|there you go`.

Flow vibe is off by default on purpose: a constant baseline habituates and
steals contrast from the phasic rewards. It's there because it's a legitimate
mode, not because it conditions better.

## Demo

`build\demo.exe` is the whole loop in a console window, no editor needed:
type prose, watch the FSM, get buzzed when you hold flow on real content.

```
demo.exe              interactive, demo-friendly timing
demo.exe --shipped    the plugin's patient shipped timing
demo.exe --selftest   synthetic typist verifies the loop end-to-end, exit 0 = pass
demo.exe --no-hw      screen only
demo.exe --vibe       enable the flow-vibe tonic channel
demo.exe --url <ws>   Intiface server (default ws://127.0.0.1:12345)
```

## Install

1. Build (below), then copy `build\SkinnerBoxPP.dll` into
   `<Notepad++>\plugins\SkinnerBoxPP\`.
2. Restart Notepad++. **Plugins → SkinnerBox++** has the enable toggle,
   config open/reload, session logs, the debug-telemetry arm, the flow-vibe
   toggle, and Intiface test-buzz/reconnect.
3. For hardware rewards: start Intiface Central and connect your device
   before (or after — the plugin reconnects) starting a session.

## Build

Requires Visual Studio Build Tools (C++ workload). From a plain shell:

```
build.bat
```

Emits the plugin DLL, `replay.exe` (offline log replay harness), and
`demo.exe`, then builds and runs the core unit tests (`unit.exe`) — a build
only succeeds if they pass.

## Debug telemetry (optional)

One plugin menu arm, **Debug telemetry REC** (never feeds rewards): a raw
per-event session log *including typed text* — the corpus for the offline
audit below. Default off; the status bar shows REC while armed. Raw text
stays local and is never committed (`experiments/data/` is gitignored).

## Experiments

`experiments/` is the offline lab — everything model-shaped happens here,
post-session, never in the plugin (the live GPT-2 arm was removed after
experiment 05 showed the runtime's own entropy facet tracks judged quality
better). Quality ground truth is **external and post-session** — never
writer self-labels mid-flow (see `docs/architecture.md`).

```
# after a session with REC on:
python experiments/audit_session.py prepare %APPDATA%\Notepad++\plugins\config\SkinnerBoxPP-logs\session-….raw.jsonl
# label windows per experiments/judge_rubric.md (external judges) -> labels.jsonl
python experiments/audit_session.py correlate data/audit/<stem>/packet.json
```

See `docs/experiment-02-quality-signals.md` for junk-gate / model-sweep
results and `docs/experiment-05-live-session-audit.md` for the first
end-to-end session retrospective.

## Layout & docs

- `src/core/` — editor-independent brain: content gate, estimator, policy,
  engine. Includes nothing from Windows or Notepad++.
- `src/adapters/` — output channels: metadata log, raw REC log, SFX,
  Intiface (Buttplug v4 WebSocket).
- `src/plugin/` — the Notepad++ shell: plugin ABI, coin overlay, caret-line
  visuals.
- `docs/architecture.md` — the loop, the boundaries, and the design record
  of what got cut and why.
- `docs/output-contract.md` — the normalized adapter interface: new reward
  channels require zero changes to estimator or policy.

## Status

Early and moving fast. `v0.1.0` is tagged, but the coin channel and a
breaking core refactor landed after it; expect tunables and interfaces to
change without ceremony. The open problem is quality measurement — tracked
in the experiment writeups, handled strictly offline.

## License

GPL v3 or later. Vendored Notepad++/Scintilla headers under `src/npp/` are
GPL from upstream.
