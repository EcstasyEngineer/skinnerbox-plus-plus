# Experiment 05 — first live session audit: runtime state vs judged quality

Question: run the whole retrospective pipeline end-to-end on one real REC
session — does the runtime state (flow, gate, facets) line up with what an
external auditor says about the writing, and what should change (in the
runtime, in the telemetry, or in the retrospective itself) before the next
one?

Session: 2026-08-02, ~18.5 min wall / ~8 min typing, 2,399 typed chars of
free-form design ramble (the reward-system pivot dump). REC armed; Intiface
connected; VI rewards on.

## Method

1. `audit_session.py prepare` on the raw log → 7 overlapping 600-char
   typed-stream windows, GPT-2-scored (fiction band center 5.68 bits/tok,
   MAD 0.56).
2. External audit per design law (architecture.md): the writer never
   self-rates. Auditor = Claude (Fable 5), **two independent judge lenses per
   window** (editor / state — see `experiments/judge_rubric.md` v1), run as
   isolated subagents so neither sees the other's scores. Fields:
   quality / energy / on_topic / junk. Aggregate = mean, junk = OR.
3. `audit_session.py correlate` labels × predictors.
4. Tick-stream alignment: per-window mean flow, %FLOW, gate pass rate, cpm,
   named gate failures (windows assigned by tick timestamp ∈ (prev t_end,
   t_end]).

## Results

Per-window runtime state × judged labels:

| win | session s | flow | FLOW% | gate% | cpm | gpt2 bits | quality | energy | gate fails |
|---|---|---|---|---|---|---|---|---|---|
| w0000 | 1–121 | 0.63 | 56% | 78% | 218 | 7.11 | 0.57 | 0.53 | thin ×26 |
| w0001 | 120–184 | 1.00 | 81% | 81% | 290 | 7.72 | 0.61 | 0.62 | echo ×12 |
| w0002 | 184–254 | 1.00 | 100% | 100% | 252 | 8.16 | 0.65 | 0.75 | — |
| w0003 | 255–321 | 1.00 | 100% | 100% | 225 | 7.73 | 0.66 | 0.76 | — |
| w0004 | 320–378 | 1.00 | 100% | 100% | 290 | 7.41 | 0.66 | 0.72 | — |
| w0005 | 378–423 | 1.00 | 100% | 100% | 340 | 8.34 | 0.65 | 0.72 | — |
| w0006 | 424–486 | 1.00 | 100% | 100% | 324 | 7.89 | 0.55 | 0.57 | — |

Regime totals: 551 s IDLE (lead-in/lead-out), 75 s TYPING, 488 s FLOW.
Gate passed 95% of active ticks. Two `flow_vi_reward` deliveries in 488 s of
FLOW. Inter-lens gaps ≤ 0.13 on every window (noise floor of the
instrument); the editor lens ran consistently ≤ the state lens but
rank-ordered windows identically.

Label × predictor correlations (n = 7 — directional only):

| label | gpt2_mean_bits | gpt2_band_dist | facet_entropy | facet_repetition |
|---|---|---|---|---|
| quality | +0.31 | +0.31 | **+0.75** | −0.09 |
| energy | +0.53 | +0.53 | **+0.77** | −0.09 |
| on_topic | −0.14 | −0.14 | +0.44 | +0.08 |

(`facet_stall` and `junk` had zero variance — no junk in the session.)

## Findings

1. **The judged arc matches the runtime arc.** Both lenses independently
   recovered the session's shape — warmup (w0000), climb, a four-window
   plateau of peak quality/energy exactly where the estimator sat pegged at
   flow 1.00 / gate 100%, and a tail-off (w0006) as the dump wound down. The
   1 Hz momentum machine and a post-hoc quality read agree about *when this
   writer was cooking*, at least on a good session.
2. **The cheap facet beat the expensive model.** Char entropy — already in
   the runtime gate — tracked judged quality (+0.75) and energy (+0.77)
   better than GPT-2 surprisal (+0.31 / +0.53). GPT-2's fiction-anchored
   band distance read the whole session as 2.5–4.7 MADs "off-band" while
   judges scored it 0.55–0.66 quality: the band answers "is this fiction
   prose", not "is this good work in its genre". This is the empirical nail
   for keeping GPT-2 out of the runtime (and this session retired the live
   LAB arm entirely).
3. **`echo` false-positived on rhetoric.** 12 s of gate loss in w0001 —
   during honest peak flow — from "a good boy or good girl or good job":
   legitimate parallel construction tripped the tail repeated-token check
   (`tail_max_token ≥ 3`). One session, one facet, one false positive, cost
   ≈ 12 s of reward eligibility. Watch across sessions before tuning; if it
   recurs, the tail check needs a function-word or rhetoric allowance, not a
   bigger threshold.
4. **Reward density was starvation-level.** 488 s of qualifying FLOW paid
   out twice (VI mean 120 s + forfeit-on-exit + cooldown). For hardware that
   also costs setup overhead, that's the "a lot of overhead" complaint in
   the dump — and the motivation for the coin channel: the yellow tier pays
   on production volume (~every 250 chars), the red tier keeps the VI
   schedule, and neither requires strapping anything on.

## Mentor cross-check (external review of "can you rate free writing?")

An outside take on the same problem (from the user's mentor, paraphrased to
its operational core): writing quality is not a scalar — it's subjective,
purpose-dependent (good copywriter ≠ good novelist), and the only objective
floor is *"can you put together a sentence."* What works is moving multiple
raters toward consensus, and progress comes from volume of doing plus
reading, with voice emerging from emulation.

That maps almost exactly onto what this repo already codified, which is a
useful independent confirmation:

| mentor's claim | this repo's mechanism |
|---|---|
| the only objective baseline is sentence-construction | the runtime junk gate (catches junk, never "bad writing") |
| quality is consensus, not a number | multi-lens external audits, averaged, disagreement flagged |
| quality is purpose-dependent | genre-anchored rubric ("quality FOR ITS GENRE") |
| progress = doing the writing | the box's whole thesis: reinforce producing; the yellow tier is literally a volume reinforcer |

One divergence worth keeping: the mentor frames progress as only learnable
by doing + reading — i.e. don't expect any instrument to certify
improvement. The retrospective's honest ambition is narrower: track
*consensus deltas on a fixed rubric across sessions*, which is a trend
line, not a verdict.

## Productionalizing the retrospective

What a re-run takes today (~15 min, one manual seam):

1. Arm REC, write, disarm.
2. `python experiments/audit_session.py prepare <session>.raw.jsonl`
3. Judge the packet windows per `experiments/judge_rubric.md` (any capable
   LLM, two lenses, isolated contexts) → `labels.jsonl`.
4. `python experiments/audit_session.py correlate <packet>` + the tick
   alignment (fold into `audit_session.py` as a `timeline` subcommand when
   it's needed a second time).

To keep it optimizable:

- **The rubric is the versioned instrument.** Rubric changes are visible in
  git; correlations across sessions are only comparable within a rubric
  version.
- **Accumulate, don't discard.** Keep every session's packet + labels under
  `experiments/data/audit/` (local, gitignored). Per-writer calibration
  (entropy band, cpm baseline, judged-quality trend) falls out of the
  accumulated set — n=7 becomes n=70 after ten sessions.
- **Telemetry worth adding**, each named to the failure mode it catches:
  - *coin lifecycle events* (spawn/collect/expire + latency): without them
    the next audit cannot check the contiguity claim — that collects land
    mid-burst, not after stalls. This is the one blocking gap the new
    channel introduces.
  - *caret jumps / navigation events*: distinguishes drafting from editing
    passes; today a revision session would look like low-momentum drafting
    and judged on_topic drift can't be localized.
  - Deliberately NOT adding: inter-key intervals, burst taxonomy, per-window
    deletion rates — all already derivable from the existing `ins`/`del`
    event timestamps; derive at analysis time instead of widening the log.

## Honest limits

n = 7 windows, one writer, one session, one genre (design ramble). Judges
are LLMs: consistent and cheap, but consensus-of-LLM-lenses is a proxy for
the external-human-auditor the design law actually names. The session's text
is *about* the tool measuring it, which plausibly inflates on_topic. And a
session where flow sat pegged at 1.00 for five minutes gives the estimator
no hard cases — the interesting audit is a bad session, which by
construction is the one nobody arms REC for. Run the retro on a grinding,
stall-heavy session before trusting finding 1.
