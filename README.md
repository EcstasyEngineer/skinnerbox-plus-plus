# SkinnerBox++

An operant conditioning chamber for your editor.

SkinnerBox++ is a self-contained Notepad++ plugin that measures two things
about your writing — **momentum** (are you typing?) and **content entropy**
(are you typing *writing*, not junk?) — and reinforces the state where both
hold. Qualifying stretches of flow earn vibration rewards through
[Intiface Central](https://intiface.com/central/) on a variable-interval
schedule. The box rewards the behavior it wants more of. You are the occupant.

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
  catch "duper duper duper" the moment it happens. No content evidence = gate
  fails closed. Momentum alone can never reach FLOW.
- **Reward policy.** Variable-interval, state-gated: hold FLOW ≥ 30 s and
  eligibility matures on an exponential hazard (mean 2 min); the reward fires
  while you're actively typing — the buzz lands during the behavior it
  reinforces, never in the pause after it — with a hard 60 s cooldown.
  VI + a state gate reinforces *staying in the state*, not performing for the
  dispenser.
- **Outputs.** The caret line warms toward gold as flow rises (tonic); a
  delivered reward buzzes your Intiface device and blooms the tint (phasic).
  Client-side intensity cap (30% of the device's range by default) — Buttplug
  has no server-side cap, so the plugin owns the stop and every buzz ends in
  an explicit zero.

## Demo

`build\demo.exe` is the whole loop in a console window, no editor needed:
type prose, watch the FSM, get buzzed when you hold flow on real content.

```
demo.exe              interactive, demo-friendly timing
demo.exe --shipped    the plugin's patient shipped timing
demo.exe --selftest   synthetic typist verifies the loop end-to-end, exit 0 = pass
demo.exe --no-hw      screen only
```

## Install

1. Build (below), then copy `build\SkinnerBoxPP.dll` into
   `<Notepad++>\plugins\SkinnerBoxPP\`.
2. Start Intiface Central, connect your device.
3. Restart Notepad++. **Plugins → SkinnerBox++** has the toggle and config.

## Build

Requires Visual Studio Build Tools (C++ workload). From a plain shell:

```
build.bat
```

Emits the plugin DLL, `replay.exe` (offline log replay harness), and
`demo.exe`.

## Experiments

`experiments/` holds the offline lab (Python, local-only data): can GPT-2
surprisal or POS-tag statistics measure writing *quality* well enough to
gate rewards on it? See `docs/experiment-02-quality-signals.md` for results.
Python is tooling only — nothing in the runtime loop depends on it.

## License

GPL v3 or later. Vendored Notepad++/Scintilla headers under `src/npp/` are
GPL from upstream.
