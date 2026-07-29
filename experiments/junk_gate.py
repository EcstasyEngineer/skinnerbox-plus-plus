# Experiment 02 — can GPT-2 tell junk from writing, and is it worth it?
#
# The question the old probe ladder (probe*.py, now deleted) failed to answer:
# "salience of the words currently being typed — do not reward junk." The old
# method regressed pooled hidden states against 39 LLM-judge labels with leaky
# CV; n was too small to mean anything and the target (judge salience) wasn't
# even the deployment question (junk gating).
#
# This version is label-free and answers the deployment question directly.
# GPT-2 is used as a *language model*, not a feature bank: per-token surprisal
# -log2 p(token | prefix) under its causal head. The working hypothesis:
#
#   repetition/loops/stopword streams -> abnormally LOW surprisal (the model
#       finds them trivially predictable)
#   keyboard mash                     -> abnormally HIGH surprisal
#   real prose                        -> a mid band
#
# so the junk score is a two-sided distance from a prose band calibrated on
# reference fiction ONLY (median/MAD — no junk labels touch the calibration).
# Report: AUC per junk class vs real windows, the same for the plugin's
# current lexical gate (exact reimplementation) as baseline, where user
# windows land, and CPU latency for a 600-char window (real-time feasibility).
import json
import math
import time
from collections import Counter
from pathlib import Path

import numpy as np

import corpus

DATA = Path(__file__).parent / "data"
RESULTS = Path(__file__).parent / "results"


# --- the plugin's lexical gate, mirrored from src/core/content.cpp ----------

def lexical_facets(text):
    hist = Counter(text)
    n = len(text)
    ent = -sum((k / n) * math.log2(k / n) for k in hist.values()) if n else 0.0
    toks = [t.lower() for t in "".join(
        c if c.isalpha() else " " for c in text).split()]
    rep = 1.0 - len(set(toks)) / len(toks) if toks else 0.0
    stall = sum(1 for t in toks if len(t) >= 3 and set(t) <= set("uhm"))
    stall_frac = stall / len(toks) if toks else 0.0
    tail = text[-80:]
    run = mx = 0
    for c in tail.lower():
        run = run + 1 if c in "uhm" else 0
        mx = max(mx, run)
    tail_toks = [t.lower() for t in "".join(
        c if c.isalpha() else " " for c in tail).split()]
    tc = Counter(t for t in tail_toks if len(t) >= 4)
    tail_max_token = max(tc.values()) if tc else 0
    return dict(entropy=ent, repetition=rep, stall_frac=stall_frac,
                tail_stall_run=mx, tail_max_token=tail_max_token,
                window_chars=n)


def lexical_gate_ok(f):
    return (f["window_chars"] >= 80 and f["repetition"] <= 0.55 and
            f["entropy"] >= 3.4 and f["stall_frac"] <= 0.06 and
            f["tail_stall_run"] < 6 and f["tail_max_token"] < 3)


def lexical_junk_score(f):
    # Continuous version of the gate for AUC comparison: worst normalized
    # violation across the same facets the C++ gate thresholds.
    return max(f["repetition"] / 0.55,
               3.4 / max(f["entropy"], 0.1),
               f["stall_frac"] / 0.06,
               f["tail_stall_run"] / 6.0,
               f["tail_max_token"] / 3.0)


# --- portable micro-LM: character-bigram log-prob ---------------------------
# The lexical gate's one hole (found below) is keyboard mash: high char
# entropy, no repetition — it looks "diverse". A 27x27 char-bigram table
# trained on any English corpus is a 3 KB language model that measures
# English-likeness directly, runs in microseconds, and ports to C++ as a
# static array. This rung asks: does it close the mash hole?

class CharBigramLM:
    ALPH = "abcdefghijklmnopqrstuvwxyz "

    def __init__(self):
        import numpy as _np
        from nltk.corpus import brown
        counts = _np.ones((27, 27))  # Laplace
        text = " ".join(" ".join(s) for s in brown.sents(categories="news"))
        prev = None
        for ch in text.lower():
            idx = self.ALPH.find(ch)
            if idx < 0:
                idx = 26  # everything non-alpha folds to space
            if prev is not None and not (prev == 26 and idx == 26):
                counts[prev, idx] += 1
            prev = idx
        self.logp = _np.log2(counts / counts.sum(axis=1, keepdims=True))

    def bits_per_char(self, text):
        prev = None
        bits, n = 0.0, 0
        for ch in text.lower():
            idx = self.ALPH.find(ch)
            if idx < 0:
                idx = 26
            if prev is not None and not (prev == 26 and idx == 26):
                bits += -self.logp[prev, idx]
                n += 1
            prev = idx
        return bits / n if n else 0.0


# --- GPT-2 surprisal --------------------------------------------------------

class Gpt2Scorer:
    def __init__(self):
        import torch
        from transformers import GPT2LMHeadModel, GPT2TokenizerFast
        self.torch = torch
        self.tok = GPT2TokenizerFast.from_pretrained("gpt2")
        self.model = GPT2LMHeadModel.from_pretrained("gpt2")
        self.model.eval()

    def surprisal_stats(self, text):
        torch = self.torch
        ids = self.tok(text, return_tensors="pt", truncation=True,
                       max_length=256).input_ids
        if ids.shape[1] < 8:
            return None
        with torch.no_grad():
            logits = self.model(ids).logits
        logp = torch.log_softmax(logits[0, :-1], dim=-1)
        tgt = ids[0, 1:]
        s = -logp[torch.arange(tgt.shape[0]), tgt] / math.log(2)  # bits
        s = s.numpy()
        return dict(mean=float(np.mean(s)), std=float(np.std(s)),
                    p10=float(np.percentile(s, 10)),
                    p90=float(np.percentile(s, 90)),
                    n_tok=int(len(s)))


def auc(pos_scores, neg_scores):
    """P(junk score > real score) via rank statistic."""
    from sklearn.metrics import roc_auc_score
    y = [1] * len(pos_scores) + [0] * len(neg_scores)
    return float(roc_auc_score(y, list(pos_scores) + list(neg_scores)))


def main():
    RESULTS.mkdir(exist_ok=True)
    cfile = DATA / "corpus.json"
    c = (json.load(open(cfile, encoding="utf-8")) if cfile.exists()
         else corpus.build(save_dir=DATA))
    for k, v in c.items():
        print(f"{k:>20}: {len(v)} windows")

    real_keys = [k for k in ("real_user", "fiction") if c.get(k)]
    junk_keys = [k for k in c if k.startswith("junk_")]

    print("\nscoring with GPT-2 124M (CPU)...")
    scorer = Gpt2Scorer()
    stats = {}
    t0 = time.perf_counter()
    n_scored = 0
    for k in real_keys + junk_keys + ["noncreative"]:
        stats[k] = [s for s in (scorer.surprisal_stats(w) for w in c[k]) if s]
        n_scored += len(stats[k])
    elapsed = time.perf_counter() - t0
    ms_per_window = 1000.0 * elapsed / max(1, n_scored)

    # Prose band calibrated on fiction only.
    fic_mean = np.array([s["mean"] for s in stats["fiction"]])
    center = float(np.median(fic_mean))
    mad = float(np.median(np.abs(fic_mean - center))) or 1.0

    def band_score(s):
        return abs(s["mean"] - center) / mad

    report = {"center_bits": center, "mad_bits": mad,
              "ms_per_600char_window_cpu": ms_per_window, "auc": {}}
    real_band = [band_score(s) for k in real_keys for s in stats[k]]
    real_lex = [lexical_junk_score(lexical_facets(w))
                for k in real_keys for w in c[k]]

    print(f"\nprose band: {center:.2f} bits/token (MAD {mad:.2f}); "
          f"{ms_per_window:.0f} ms/window on CPU")
    print(f"\n{'junk class':>20}  {'GPT-2 band AUC':>14}  {'lexical AUC':>11}  "
          f"{'gate catches':>12}")
    for k in junk_keys:
        jb = [band_score(s) for s in stats[k]]
        jl = [lexical_junk_score(lexical_facets(w)) for w in c[k]]
        caught = sum(1 for w in c[k] if not lexical_gate_ok(lexical_facets(w)))
        a_gpt = auc(jb, real_band)
        a_lex = auc(jl, real_lex)
        report["auc"][k] = {"gpt2_band": a_gpt, "lexical": a_lex,
                            "lexical_gate_catch_rate": caught / len(c[k])}
        print(f"{k:>20}  {a_gpt:>14.3f}  {a_lex:>11.3f}  "
              f"{caught:>5}/{len(c[k])}")

    # Where do the sources sit in surprisal space? (sanity + interpretation)
    print(f"\n{'source':>20}  {'mean bits/tok':>13}  {'band dist':>9}")
    for k in real_keys + ["noncreative"] + junk_keys:
        m = np.mean([s["mean"] for s in stats[k]])
        b = np.mean([band_score(s) for s in stats[k]])
        report.setdefault("surprisal_by_source", {})[k] = {
            "mean_bits": float(m), "band_dist": float(b)}
        print(f"{k:>20}  {m:>13.2f}  {b:>9.2f}")

    # False-positive check: how much REAL writing would a band gate flag?
    for thr in (3.0, 5.0, 8.0):
        fp = np.mean([b > thr for b in real_band])
        report.setdefault("real_flagged_at_band_thr", {})[str(thr)] = float(fp)
        print(f"band>{thr:.0f}: flags {100*fp:.1f}% of real windows")

    # --- char-bigram rung: does a 3 KB model close the mash hole? ---
    lm = CharBigramLM()
    real_bpc = [lm.bits_per_char(w) for k in real_keys for w in c[k]]
    print(f"\nchar-bigram bits/char — real: mean {np.mean(real_bpc):.2f} "
          f"p99 {np.percentile(real_bpc, 99):.2f}")
    for k in junk_keys:
        jb = [lm.bits_per_char(w) for w in c[k]]
        a = auc(jb, real_bpc)
        report.setdefault("char_bigram", {})[k] = {
            "auc": a, "mean_bpc": float(np.mean(jb))}
        print(f"{k:>20}: AUC {a:.3f}  mean {np.mean(jb):.2f} bits/char")
    thr = float(np.percentile(real_bpc, 99.5))
    mash_caught = np.mean([lm.bits_per_char(w) > thr
                           for w in c.get("junk_keyboard_mash", [])])
    report["char_bigram_threshold"] = {
        "bits_per_char": thr, "mash_catch_rate": float(mash_caught),
        "real_false_positive": 0.005}
    print(f"threshold {thr:.2f} bits/char catches "
          f"{100*mash_caught:.0f}% of mash at 0.5% real FP")

    with open(RESULTS / "junk_gate.json", "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"\nwrote {RESULTS / 'junk_gate.json'}")


if __name__ == "__main__":
    main()
