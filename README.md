# SkinnerBox++

An operant conditioning chamber for your editor.

SkinnerBox++ is a self-contained Notepad++ plugin that watches your writing
telemetry — typing bursts, deletion ratios, momentum, stalls, focus departures —
estimates whether you're in flow, and reinforces that state. The editor warms
around you while flow holds; qualifying moments earn a chime and a bloom. The
box rewards the behavior it wants more of. You are the occupant.

No cloud, no daemon, no second thing to launch. One DLL, installed like any
other plugin. Session logs are **metadata-only** — behavioral features and
reward events, never your document text.

## How it works

```
editor events ─→ feature extraction ─→ flow estimate ─→ reward policy ─→ outputs
```

- **Telemetry.** Scintilla modification events (user-performed inserts/deletes),
  idle time, focus departures. Aggregated into rolling windows: net chars/min,
  deletion ratio, current burst length.
- **Flow estimate.** A smoothed 0–1 score with hysteresis (enter FLOW at 0.70,
  leave below 0.50) and four regimes: `DRAFTING`, `FLOW`, `EDITING`, `STALL`.
  Thinking pauses aren't punished; only long idleness reads as a stall.
- **Reward policy.** State-gated **variable-interval** reinforcement: while flow
  holds (90 s minimum), reward eligibility matures on an exponential hazard
  (mean 7 min), fires only at a natural pause, and respects a hard 4-minute
  cooldown. Recovering from a stall with real forward progress earns its own
  reward. A configurable fraction of qualifying moments is silently withheld
  and logged — counterfactual data for the future adaptive policy.
- **Outputs.** Tonic: the caret line warms toward gold as flow rises; the
  status bar shows a small meter. Phasic: a soft chime plus a brief brighter
  bloom on delivered rewards. All output channels implement one adapter
  contract — see [docs/output-contract.md](docs/output-contract.md) for how
  physical reward hardware plugs in later.

## Install

1. Grab `SkinnerBoxPP.dll` (build it — see below).
2. Create `<Notepad++ install>\plugins\SkinnerBoxPP\` and drop the DLL in it.
3. Restart Notepad++. A `SkinnerBox++` entry appears under **Plugins**.

## Build

Requires Visual Studio Build Tools (C++ workload). From a plain shell:

```
build.bat
```

Output lands at `build\SkinnerBoxPP.dll` (x64).

## Configure

**Plugins → SkinnerBox++ → Open config** opens the INI (thresholds, intervals,
channel toggles). Edit, save, then **Reload config**. Everything documented in
`src/core/config.h` is exposed.

**Open session logs** opens the JSONL session directory. One file per session:
regime transitions, 30-second feature snapshots, and reward events (including
withheld ones).

## Roadmap

- Physical reward adapters over MCP (candy dispenser, haptics — see the
  output contract)
- Personal salience model: label passages, train a ranker, gate rewards on
  "was that bit actually good"
- Audio clarity/muffling as a tonic channel
- N-of-1 crossover experiment tooling on the session logs

## License

GPL-3.0-or-later. Vendored Notepad++ plugin headers are GPL, © Don HO.
