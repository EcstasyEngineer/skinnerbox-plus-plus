# Session-audit judge rubric (v1)

The external-auditor prompt used for labeling audit-packet windows
(`audit_session.py prepare` → `labels.jsonl`). Versioned here so every
retrospective uses the same instrument and label drift is visible in git.
Change the rubric → bump the version → note it in the experiment writeup.

## Context given to every judge

> You are an external post-session auditor for a writing-conditioning
> experiment. You are scoring a 600-char window of the TYPED STREAM: every
> character in the order it was typed, with deletions NOT applied. Correction
> artifacts appear as interleaved fragments (e.g. "buggttplug" = typo +
> retype). Do NOT penalize the mechanical artifacts themselves; DO consider
> what survives: the prose being produced.

Plus one sentence naming the session's apparent thread (from a skim of the
packet), so `on_topic` has a referent.

## Fields (match `audit_session.py` label_fields)

- `quality` — 0–1 overall writing quality FOR ITS GENRE (live design
  thinking is not judged as polished fiction). Anchors: 0.2 barely coherent
  fragments · 0.5 readable but meandering, low idea density · 0.8 sharp,
  specific, ideas connect and advance the work.
- `energy` — 0–1 alive/generative energy. Anchors: 0.2 stalling, hedging in
  circles · 0.5 steady but flat · 0.8 ideas arriving faster than the typing,
  building on themselves.
- `on_topic` — 0–1 still on the session's apparent thread.
- `junk` — 1 ONLY if the window is gaming/slop/filler rather than real prose.
- `note` — one sentence on the window's dominant character.

## Lenses (two independent judges per window, averaged)

1. **editor** — a demanding line editor: craft, sentence construction,
   specificity, idea density, is the thinking advancing or circling.
2. **state** — reads cognitive state from text: generative momentum, live
   decision-making vs treading water; hedging spirals lower energy, concrete
   decisions and fresh images raise it.

Aggregation: mean per field; `junk` = OR. Flag any window where the two
lenses disagree by > 0.3 (re-judge with a third lens instead of averaging a
dispute away). Report the max per-window gap in the writeup — it is the
instrument's noise floor.
