# Experiment 01 — does text salience add signal over typing telemetry?

Question (issue #1): is LLM-judged text salience worth building toward (up to
and including an optional 1–3 GB local model), or is the telemetry-only
estimator the right long-term core?

Two sessions of live stream-of-consciousness writing by the primary user, with
the plugin running. Session A (~20 min) had only the standard metadata log
(30 s snapshots); session B (~5 min) had the raw per-event log with text
capture. No document text is reproduced in this doc; quantitative results only.

## Method

**Session A (coarse):** text was aligned to the snapshot timeline by
integrating net-cpm per window (integration landed within 3 chars of true
length; validated against in-text anchors where the writer narrated the meter
value — all anchors mapped to the windows whose logged values matched).
39 window-chunks were scored by three LLM judges of different sizes
(small/mid/large), blind to all telemetry, on salience / on-topic / energy
(0–1). Scores were then correlated against the aligned telemetry.

**Session B (fine):** the raw log's insert/delete events reconstructed the
document exactly (1717/1717 chars), giving per-character timestamps, exact
pause locations, and per-token flow lookup.

## Results

| Measure | Value | Reading |
|---|---|---|
| Inter-judge agreement (salience) | r = 0.87–0.95; smallest↔largest 0.91 | the label is stable across prompted judges of different sizes — reliability, NOT proof a 1–3 GB local model can recover it (no local model was run) |
| Same-window r(salience, flow) | 0.40–0.47 | correlated but ~20% shared variance — new information, not a flow proxy |
| High-flow/low-salience windows | flow 0.90 @ 370 cpm scored 0.10–0.15 salience | telemetry cannot distinguish idea-birth from fast filler |
| Pre-stall salience (natural thread-loss) | 0.20–0.30 vs ~0.5 session mean, all judges | salience dips *before* the stall telemetry sees |
| Pre-pause salience (retrieval pause) | 0.72–0.80 | the opposite sign — salience separates pause types (see pause-taxonomy.md) |
| Next-window pause prediction | r = −0.06 to −0.21 (n≈36, ns) | too weak at 30 s granularity; needs fine-grained windows |
| Adversarial probe (session B) | deliberate filler tokens all sat at flow = 1.00 | momentum facets are fully spoofable by low-content typing |

## Conclusions

(Revised after adversarial review — original phrasing of 1–3 was overclaimed;
see "Adversarial review" below.)

1. **Salience labels are stable across prompted LLM judges and carry
   information the telemetry doesn't** (r ≈ 0.45 with flow on this N-of-1
   sample). NOT established: construct validity against human/self labels,
   or that any local 1–3 GB model can recover the label — no local model or
   embedding baseline was run. Reliability ≠ deployability.
2. **The most promising use is disambiguation, not scoring**: in these
   sessions, salience separated thinking-pauses from thread-loss and filler
   from content — cases where the *current* telemetry was blind. Whether
   richer telemetry alone (resume location, post-resume persistence, edit
   patterns) could do the same discrimination is untested.
3. **Chunk-level judging buries filler** — filler exists to prevent pauses, so
   pause/window chunks hide it. Hypothesis (untested): cheap lexical signals
   (repetition, entropy) catch much of it with zero model download; build and
   re-run the filler probe against them before any model work.
4. **Not yet shown:** that salience-gated rewards improve outcomes (continued
   productive writing after reward). That requires the withheld-counterfactual
   comparison across more sessions — the log already collects it. **This is
   the gating conclusion: no new reward semantics, models, or modes until the
   withholding data shows conditioning helps.**

## Adversarial review

Two independent reviewers (different model families) attacked this writeup and
the open backlog. Both returned the same top findings: (a) judge agreement was
being read as local-model feasibility — wrong, no local model was evaluated;
(b) the architecture doc claimed a conjunction the estimator doesn't implement
— corrected; (c) the project was specifying stronger conditioning (retrieval
bonuses, punishment, hardware) ahead of any evidence that conditioning
improves outcomes — backlog re-gated on conclusion 4. Pause-handling design
implications are recorded in pause-taxonomy.md.

## Next

- Fit pause-taxonomy constants from raw-log gap distributions.
- Add the lexical anti-slop facet (no model needed) and re-run the adversarial
  filler probe against it.
- Accumulate withheld-vs-delivered outcome data before any policy learning.
