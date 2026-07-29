# Architecture

Design rule: **one install, modular internals.** Everything ships as a single
Notepad++ plugin, but the flow logic is an editor-independent library so it can
be rehosted (the console demo is a second host) without rewriting the brain.

```
src/
├── npp/        vendored Notepad++/Scintilla plugin headers (GPL, upstream)
├── core/       editor-independent: the brain
│   ├── reward_intent.h   RewardIntent / AmbientState / the FSM definition
│   ├── adapter.h         IOutputAdapter — one interface per reward channel
│   ├── config.h          all tunables, one struct
│   ├── content.*         typed-stream window: entropy + anti-slop facets
│   ├── estimator.*       rolling-window momentum + EWMA score + the FSM
│   ├── policy.*          state-gated variable-interval reward policy
│   └── engine.h          events → gate → estimator → policy → adapters, 1 Hz
├── adapters/   host-agnostic output channels
│   ├── log_adapter.*     metadata-only JSONL session log
│   ├── raw_log.*         opt-in debug telemetry (typed text; experiment data)
│   └── intiface_adapter.* Buttplug v4 WebSocket client — THE hardware channel
├── plugin/     Notepad++-specific shell
│   ├── plugin_main.cpp        plugin ABI, timer, INI config, log paths
│   └── npp_visual_adapter.*   caret-line tint/bloom + status-bar meter
test/
├── replay.cpp  replays a raw session log through the real engine
└── demo.cpp    interactive console host: keystrokes → engine → Intiface
```

## The loop

Everything the plugin does is this one sentence: **measure whether the writer
is producing (momentum) and whether what they produce is writing (content
entropy), and afford vibration rewards while both hold.**

The FSM (defined in `reward_intent.h`, implemented in ~10 lines in
`estimator.cpp`):

- `IDLE` — no input for 30 s. Nothing measured, nothing owed.
- `TYPING` — producing input; momentum or content hasn't qualified yet.
- `FLOW` — smoothed momentum score above threshold (hysteresis 0.70/0.50)
  **and** the content gate passes. The only state where rewards mature.

The policy (in `policy.cpp`, one trigger): hold FLOW ≥ `min_flow_hold_s`,
eligibility matures on an exponential hazard (`mean_reward_interval_s`),
fires on the next actively-typing tick (idle < 1 s), hard `min_cooldown_s`
floor between deliveries. Leaving FLOW forfeits eligibility.

## Boundaries that matter

- `core/` includes nothing from `npp/` and nothing from `<windows.h>`. Hosts
  feed it raw events (`on_insert`, `on_delete`, `on_focus_loss`) and a
  monotonic ~1 Hz `tick(now)`.
- The policy never speaks device language. It emits `RewardIntent` (semantic
  class + abstract 0–1 dose); adapters map that to their channel. See
  [output-contract.md](output-contract.md).
- Adapters fail toward off/neutral: `shutdown()` restores anything they
  touched; the Intiface adapter explicitly zeroes every buzz and sends
  `StopAllDevices` on shutdown.
- The content gate is a **conjunction**, not a blended score: momentum can
  never buy back a failed gate. An adversarial session proved the momentum
  blend alone is fully spoofable by filler typing (see
  experiment-01-salience.md); the gate is the answer.

## What got cut, and why (design record)

The following shipped at some point and was deliberately removed. Don't
reinvent it without new evidence:

- **Stall-recovery rewards** — paying out for coming back after a stall
  reinforces the stall/return cycle, not writing. Cut; returning writers earn
  rewards the same way as everyone: hold FLOW.
- **Earned tonic restoration** — a second accumulator that existed only to
  soften the recovery-reward design. Died with it; tonic warmth now just
  tracks flow.
- **Withholding / counterfactual arms** (per-moment and block-randomized) —
  infrastructure for a future adaptive-policy study, bolted on before the MVP
  worked. An experiment worth running someday is not a feature worth carrying
  in the loop today.
- **PAUSED/EDITING/STALL regimes** — five states whose transition rules were
  smeared across estimator and policy. The EWMA's honest decay plus
  FLOW-hysteresis does the same job with three states.
- **MCP hardware adapter + bespoke preset backend** — a second hardware
  interface with its own protocol and server. Intiface already owns device
  support; one hardware path only.
- **Audio chime adapter** — sound interrupts reading; it was shipped
  default-off and nothing missed it.

## Policy notes

Variable-interval, not variable-ratio: a ratio schedule on countable output
(words, paragraphs) trains producing countable output. VI + a state gate says
"rewards become available unpredictably in time, but only while you're
actually in the target state," which reinforces *staying in the state* instead
of *performing for the dispenser*.

Delivery timing is contiguity-first: the buzz fires only while keys are
actively moving. The first live test ran the opposite rule (fire in the next
natural pause, inherited from "don't interrupt reading" visual-channel
thinking) and the writer — typing continuously — got the buzz seconds after
deliberately stopping, and immediately read it as "stopping pays." The
machine's contingency is irrelevant; the perceived one is what conditions.
Vibration doesn't interrupt typing, so it lands mid-behavior.

The hardware reward itself is an erogenous reinforcer, not a status buzz:
enveloped (ease-in / sustain / ease-out), fixed peak under the cap, magnitude
carried by sustain duration. Rationale and the external design review behind
it are summarized in [output-contract.md](output-contract.md) §3.

The open measurement problem is **quality**: the lexical gate catches junk,
not badness. The experiments in `experiments/` (GPT-2 surprisal band, POS-tag
statistics) are the candidate quality signals — results in
[experiment-02-quality-signals.md](experiment-02-quality-signals.md).
