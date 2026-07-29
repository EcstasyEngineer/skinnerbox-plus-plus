# Pause taxonomy (direction doc)

Status: design thinking, revised after two-model adversarial review. The first
draft's headline mechanisms (frozen-score suspension, first-phrase retrieval
rewards) were found FATALLY gameable and are recorded below as rejected, with
the exploits that killed them — they are part of the design record precisely so
they don't get reinvented.

## The core asymmetry

Creative thinking should not be a treadmill walk. It's a hike — sometimes you
boulder for four minutes to gain ten feet, and that must not be punished.
Spending ten hours to gain ten feet must not be rewarded. The current
estimator can't tell these apart because it scores idleness itself, when most
of the information is in **what surrounds the pause**.

## Observed pause classes

From session data so far, with the signals that distinguish them:

| Class | Duration | In-editor? | What follows | Example from data |
|---|---|---|---|---|
| Rhythm gap | <2 s | yes | typing continues mid-clause | (constant background) |
| Retrieval | 2–30 s | yes | the *searched-for thing* lands, same clause/thread | 15.8 s reaching for a word, followed by an above-mean-salience window |
| Reflective | 30 s – few min | yes | new paragraph/idea, often a thread pivot | 6.1 s pause → birth of a new tool idea |
| Inspection | 2–10 s | yes | commentary about the environment, not the thread | 5.8 s mid-sentence watching the reward bloom |
| Thread loss | 30 s+ | yes | weak restart, meta-narration, shallow content | a stall preceded by a below-mean-salience window |
| Exit | any | no (focus lost) | context switch | focus-loss events |
| Depletion | terminal | either | session ends | deliberate stop |

Empirical basis (experiment-01): text salience before the pause separates
retrieval/reflective (0.72–0.80) from thread-loss (0.20–0.30 vs ~0.5 mean).
Telemetry alone saw identical idle.

## Design principles (revised)

**1. Duration is classified retroactively; feedback is not.** The first draft
claimed "you cannot know what kind of pause is happening while it happens" —
false as a universal (pre-pause salience already carries class signal), and
more importantly, feedback delivered *during* a pause is reinforcement that a
later reclassification cannot retract. Five minutes of held warmth during what
turns out to be a stall is five minutes of rewarding a stall.

**2. Unresolved pause → neutral output.** While a pause is unresolved, tonic
feedback moves to *neutral* — not held at the pre-pause level, not punished.
Restoration happens only after the resume resolves the pause favorably.
Uncertainty is represented as neutrality, never as preserved reward.

**3. Never pay for the pause; be careful even paying for the landing.**
Rewarding the first phrase after a pause trains manufactured pauses and staged
landings (compose offline → pause → dump). Provenance of a resumed phrase is
unknowable from the editor. Therefore pause-linked *phasic* rewards are out
until outcome data (experiment-01, conclusion 4) justifies revisiting — and
any future version must reward sustained post-resume continuation, never the
landing itself.

## What survives, in build order

### 1. Decay grace (build)
While focus stays in-editor, flow decay doesn't begin until idle exceeds a
grace window (~8–10 s; fit from raw-log gap distributions). Exit pauses decay
immediately. Cheap, low exploit surface (worst case: grace-edge keepalive
typing, which the lexical anti-slop facet catches later). This alone delivers
most of "don't punish bouldering."

### 2. Neutral hold with active-time accounting (build after 1)
Beyond grace, tonic goes neutral (not dimmed, not held). Flow restoration on
resume is *earned by post-resume content* — sustained forward progress over a
real window (tens of seconds, cumulative active-time, not a single qualifying
burst) — never granted retroactively. Cumulative accounting prevents the
rolling-reset exploit (idle forever, sliced into sub-cap episodes by minimal
qualifying continuations).

### 3. Pause labeling in the log (build anytime, zero risk)
Log every pause with duration, focus continuity, resume location delta, and
post-resume progress — labeled pause dataset for the eventual learned policy.
Labels only; no behavior change. Note the poisoning caveat: any label derived
from gameable signals inherits the gaming; keep raw features, not just
inferred classes.

### 4. Pre-pause salience as an *arming* signal (needs content facet)
Pre-pause content quality arms a provisional class (likely-retrieval vs
likely-thread-loss) that biases how fast tonic goes neutral. This is the
salience integration point — as a modifier of feedback *withdrawal speed*,
never as a phasic payout.

## Rejected mechanisms (do not reinvent)

- **Frozen-score suspension** ("SUSPENDED keeps flow at pre-pause value"):
  FATAL — freeze-at-peak (pin the spoofable score with filler, idle, restore),
  rolling-cap reset (a 5-min cap just slices infinite idle into episodes), and
  irretractable reinforcement (held-high tonic during real stalls). Uncertainty
  must be neutral, not preserved-high.
- **First-phrase retrieval reward**: FATAL — instrumental pausing (withhold a
  known-good phrase, pause, land it), false provenance (offline/clipboard
  composition), phrase-boundary gaming, pause-frequency farming. "Was the word
  worth the wait" cannot be verified from inside the editor.
- **Punishment keyed to raw idle or flow thresholds**: punishes thinking;
  any aversive mode requires a *validated* thread-loss classifier plus
  evidence that punishment improves later writing (none exists), and must
  key off resolved thread-loss at most.

## Open questions

- Fit grace/window constants from raw-log pause distributions.
- Does neutral-hold change pause behavior itself (observer effect)?
- "Same location" needs document identity + semantic continuation, not just
  a character-distance radius — geometry alone is launderable.
