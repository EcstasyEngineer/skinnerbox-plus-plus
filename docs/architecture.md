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
│   ├── sfx.*             reinforcement SFX engine (synth bells, .wav override)
│   └── intiface_adapter.* Buttplug v4 WebSocket client — optional hardware
├── plugin/     Notepad++-specific shell
│   ├── plugin_main.cpp        plugin ABI, timer, INI config, log paths
│   ├── npp_visual_adapter.*   caret-line tint/bloom + status-bar meter
│   └── coin_overlay.*         coin rewards: spawn-ahead / collect-by-typing
test/
├── replay.cpp  replays a raw session log through the real engine
└── demo.cpp    interactive console host: keystrokes → engine → Intiface
```

## The loop

Everything the plugin does is this one sentence: **measure whether the writer
is producing (momentum) and whether what they produce is writing (content
entropy), and afford rewards while both hold** — coins ahead of the caret by
default, Intiface vibration as the optional hardware channel.

The FSM (defined in `reward_intent.h`, implemented in ~10 lines in
`estimator.cpp`):

- `IDLE` — no input for 30 s. Nothing measured, nothing owed.
- `TYPING` — producing input; momentum or content hasn't qualified yet.
- `FLOW` — smoothed momentum score above threshold (hysteresis 0.70/0.50)
  **and** the content gate passes. The only state where rewards mature.

The policy (in `policy.cpp`) has two tiers, mirroring how a platformer
prices coins:

- **Quality ("red coin", `MicroReward`)** — hold FLOW ≥ `min_flow_hold_s`,
  eligibility matures on an exponential hazard (`mean_reward_interval_s`),
  fires on the next actively-typing tick (idle < 1 s), hard `min_cooldown_s`
  floor. Leaving FLOW forfeits eligibility. The potent tier: gate required,
  rare, carries the affirmation and (if enabled) the hardware buzz.
- **Regularity ("yellow coin", `RegularityCoin`)** — fixed-ratio on net
  typed chars (`coin_yellow_interval_chars`, default 250) while not IDLE.
  Deliberately gate-free: it reinforces *producing at all*; the quality tier
  reinforces *producing well*. Known trade: volume without the gate is
  spoofable by filler (experiment 01) — accepted because the tier's payoff
  is small and the potent tier stays gated.

Delivery is the coin overlay (plugin side): the intent spawns a coin ahead
of the caret and the reinforcing moment is the *collect*, which can only
happen by typing into it.

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
- **Audio chime adapter (v1)** — cut as "sound interrupts reading", shipped
  default-off, unmissed. **Partially reversed** by the coin channel's SFX on
  new evidence: the v1 sound was a system alert (read as punishment) fired
  at delivery time; the v2 sound is a game-like reinforcer fired at the
  *collect* moment the writer produces by typing. The design record's real
  lesson stands: never an aversive or interrupting sound — not "never
  sound".
- **Live GPT-2 LAB arm** — an in-plugin bridge to a Python torch host
  streaming surprisal into the status bar. Removed after experiment 05: on a
  real session the runtime's own char-entropy facet tracked judged quality
  better than GPT-2 surprisal, and the fiction-anchored band misreads
  ramble-genre work wholesale. GPT-2 remains an offline audit instrument
  (`experiments/`); nothing model-shaped runs in-process. The removed
  integration lives in git history (checkpoint commit before the pivot) if
  new evidence ever justifies resurrecting it.

## Policy notes

Variable-interval, not variable-ratio: a ratio schedule on countable output
(words, paragraphs) trains producing countable output. VI + a state gate says
"rewards become available unpredictably in time, but only while you're
actually in the target state," which reinforces *staying in the state* instead
of *performing for the dispenser*.

Delivery timing is contiguity-first: the intent fires only while keys are
actively moving. The first live test ran the opposite rule (fire in the next
natural pause, inherited from "don't interrupt reading" visual-channel
thinking) and the writer — typing continuously — got the buzz seconds after
deliberately stopping, and immediately read it as "stopping pays." The
machine's contingency is irrelevant; the perceived one is what conditions.

The coin overlay strengthens the same property structurally: the payoff
moment is the *collect*, and the only way to collect is to type to the
coin's document position. Anticipation at spawn, reinforcement mid-behavior
by construction, and an uncollected coin expires worthless — paying it out
on return from a stall would reinforce the stall/return cycle (the same law
that killed stall-recovery rewards).

The hardware reward itself is an erogenous reinforcer, not a status buzz:
enveloped (ease-in / sustain / ease-out), fixed peak under the cap, magnitude
carried by sustain duration. Rationale and the external design review behind
it are summarized in [output-contract.md](output-contract.md) §4.

## Quality measurement (design law)

The lexical gate catches **junk**, not **bad writing**. Quality is an open
measurement problem and it is handled **offline**:

1. **External audits only.** Ground truth for quality comes from post-session
   external review (batch judges, a separate model pass, a human auditor —
   anyone but the writer mid-flow). The writer never self-rates quality during
   a session. Interrupting generative headspace for "is this good?" is
   adversarial to the state the box is training. Preference shortcuts and
   in-editor quality prompts are out, not deferred.

2. **Model-shaped analysis is 100% offline.** Surprisal numbers never enter
   the reward policy — and since the LAB-arm removal, never enter the
   *process*. The plugin has exactly one lab switch: **REC** (Debug
   telemetry), the raw per-event log **with typed text** that feeds the
   offline audit. GPT-2 lives only in `experiments/` (venv, weights in the
   local HF cache) as the audit instrument. Empirical basis:
   experiment 02 (302 ms/window; fiction band needs per-user calibration)
   and experiment 05 (char entropy out-correlated GPT-2 against judged
   quality on a live session).

3. **Raw text stays local and opt-in.** REC is the only text capture.
   Session text never ships with the repo (`experiments/data/` is
   gitignored). Offline audit: `experiments/audit_session.py` + the judge
   rubric (`experiments/judge_rubric.md`) +
   [experiment-05-live-session-audit.md](experiment-05-live-session-audit.md).

Early chat-export brainstorms are not kept in-tree. Durable decisions live
here, in the output contract, and in the experiment writeups.

## Open measurement (status)

Quality correlation against external audits is the open problem. The first
live audit (experiment 05) ran the full loop — REC → windows → two-lens
LLM judges → correlate — and found the runtime's own char-entropy facet the
best in-genre quality proxy so far (r ≈ +0.75, n = 7). The retrospective is
now repeatable (versioned rubric, accumulate packets per session); the
missing telemetry for the next round is coin lifecycle events
(spawn/collect/expire) to verify collect-contiguity, and caret-jump events
to separate drafting from editing passes.
