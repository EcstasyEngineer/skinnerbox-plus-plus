# Architecture

Design rule: **one install, modular internals.** Everything ships as a single
Notepad++ plugin, but the flow logic is an editor-independent library so it can
be rehosted (another editor, a standalone daemon, an MCP server) without
rewriting the brain.

```
src/
├── npp/        vendored Notepad++/Scintilla plugin headers (GPL, upstream)
├── core/       editor-independent: the brain
│   ├── reward_intent.h   RewardIntent / AmbientState — the output contract
│   ├── adapter.h         IOutputAdapter — one interface per reward channel
│   ├── config.h          all tunables, one struct
│   ├── estimator.*       rolling-window features + EWMA flow + regime hysteresis
│   ├── policy.*          state-gated variable-interval reward policy
│   └── engine.h          telemetry → estimator → policy → adapters, 1 Hz
├── adapters/   host-agnostic output channels
│   ├── log_adapter.*     metadata-only JSONL session log (also the dataset)
│   └── audio_adapter.*   phasic system chime (Windows)
└── plugin/     Notepad++-specific shell
    ├── plugin_main.cpp        plugin ABI, timer, INI config, log paths
    └── npp_visual_adapter.*   caret-line tint/bloom + status-bar meter
```

## Boundaries that matter

- `core/` includes nothing from `npp/` and nothing from `<windows.h>`. The
  host feeds it raw events (`on_insert`, `on_delete`, `on_focus_loss`) and a
  monotonic ~1 Hz `tick(now)`; it returns state and emits intents.
- The policy layer never speaks device language. It emits `RewardIntent`
  (semantic class + abstract 0–1 dose); each adapter decides what that means
  for its channel. See [output-contract.md](output-contract.md).
- Adapters fail open: `shutdown()` restores anything they touched (the visual
  adapter captures the theme's caret-line color at start and puts it back).
- Withheld intents (`withheld == true`) reach every adapter, but only
  logging-type adapters may act on them. Sensory channels stay silent — that's
  the counterfactual arm of the experiment.

## Two-channel output model

- **Tonic** (`ambient()`, every tick): slow environment weather keyed to the
  smoothed flow estimate. Low amplitude, heavily smoothed, reversible.
- **Phasic** (`deliver()`, discrete): reward events at meaningful moments,
  gated by the policy's interval/cooldown/boundary logic.

Habituation is fought with timing and modality rotation, never with intensity
escalation.

## Estimator notes

The flow score is currently a **weighted blend** (momentum + burst persistence,
penalized by destructive-editing ratio and focus departures, starved by
idleness) — and an adversarial probe proved the blend is spoofable: deliberate
low-content filler pinned it at 1.00 (see experiment-01-salience.md). Turning
reward eligibility into a true conjunction with must-pass content gates is
issue #3, and until that lands the score should be read as "typing momentum,"
not "flow." Regime hysteresis (0.70 enter / 0.50 exit) prevents boundary
flutter from double-firing the policy.

## Policy notes

Variable-interval, not variable-ratio: a ratio schedule on countable output
(words, paragraphs) trains producing countable output. VI + a state gate says
"rewards become available unpredictably in time, but only while you're actually
in the target state," which reinforces *staying in the state* instead of
*performing for the dispenser*. Rewards fire at burst boundaries so delivery
lands in natural pauses rather than mid-sentence.
