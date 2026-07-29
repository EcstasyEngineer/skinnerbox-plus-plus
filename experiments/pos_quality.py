# Experiment 03 — do POS-tag statistics measure creative-writing "goodness"?
#
# Two claims tested, both with honest protocols:
#
#   1. Junk robustness: does a POS-distribution distance from a fiction
#      reference catch the junk classes (same corpus and AUC protocol as
#      junk_gate.py)? Calibrated on fiction only, no junk labels.
#   2. Creative signature: can POS statistics alone separate creative prose
#      (Gutenberg fiction) from competent non-creative prose (Brown
#      government/learned)? 5-fold grouped CV logistic AUC — groups are the
#      source documents so no book leaks across folds.
#
# POS tagging is NLTK's averaged perceptron (universal tagset). If this rung
# wins, the perceptron's weights export to a flat file and reimplement in
# ~200 lines of dependency-free C++ — unlike GPT-2, this CAN live in the
# plugin's per-tick budget.
import json
import math
from collections import Counter
from pathlib import Path

import numpy as np

import corpus

DATA = Path(__file__).parent / "data"
RESULTS = Path(__file__).parent / "results"

TAGS = ["NOUN", "VERB", "ADJ", "ADV", "PRON", "DET", "ADP", "NUM",
        "CONJ", "PRT", ".", "X"]


def ensure_nltk():
    import nltk
    for res in ("punkt_tab", "averaged_perceptron_tagger_eng",
                "universal_tagset", "gutenberg", "brown"):
        try:
            nltk.data.find(f"corpora/{res}")
        except LookupError:
            try:
                nltk.data.find(f"taggers/{res}")
            except LookupError:
                try:
                    nltk.data.find(f"tokenizers/{res}")
                except LookupError:
                    nltk.download(res, quiet=True)


def pos_features(text):
    import nltk
    toks = nltk.word_tokenize(text)
    if len(toks) < 20:
        return None
    tags = [t for _, t in nltk.pos_tag(toks, tagset="universal")]
    n = len(tags)
    dist = Counter(tags)
    p = np.array([dist.get(t, 0) / n for t in TAGS])
    bigrams = Counter(zip(tags, tags[1:]))
    bn = max(1, len(tags) - 1)
    bg_ent = -sum((k / bn) * math.log2(k / bn) for k in bigrams.values())
    content = sum(dist.get(t, 0) for t in ("NOUN", "VERB", "ADJ", "ADV")) / n
    words = [w for w in toks if w.isalpha()]
    ttr = len(set(w.lower() for w in words)) / len(words) if words else 0.0
    mwl = np.mean([len(w) for w in words]) if words else 0.0
    return {"dist": p, "bigram_entropy": bg_ent, "content_ratio": content,
            "ttr": ttr, "mean_word_len": float(mwl),
            "feat_vec": np.concatenate(
                [p, [bg_ent, content, ttr, mwl / 10.0]])}


def kl(p, q, eps=1e-4):
    p = (p + eps) / (p + eps).sum()
    q = (q + eps) / (q + eps).sum()
    return float(np.sum(p * np.log2(p / q)))


def auc(pos_scores, neg_scores):
    from sklearn.metrics import roc_auc_score
    y = [1] * len(pos_scores) + [0] * len(neg_scores)
    return float(roc_auc_score(y, list(pos_scores) + list(neg_scores)))


def main():
    ensure_nltk()
    RESULTS.mkdir(exist_ok=True)
    cfile = DATA / "corpus.json"
    c = (json.load(open(cfile, encoding="utf-8")) if cfile.exists()
         else corpus.build(save_dir=DATA))

    print("tagging windows...")
    feats = {k: [f for f in (pos_features(w) for w in v) if f]
             for k, v in c.items()}
    for k, v in feats.items():
        print(f"{k:>20}: {len(v)} tagged")

    report = {}

    # --- claim 1: junk robustness via KL to the fiction POS reference ---
    ref = np.mean([f["dist"] for f in feats["fiction"]], axis=0)
    real_keys = [k for k in ("real_user", "fiction") if feats.get(k)]
    real_kl = [kl(f["dist"], ref) for k in real_keys for f in feats[k]]
    print(f"\n{'junk class':>20}  {'POS-KL AUC':>10}")
    for k in sorted(feats):
        if not k.startswith("junk_"):
            continue
        jk = [kl(f["dist"], ref) for f in feats[k]]
        a = auc(jk, real_kl)
        report.setdefault("junk_auc_pos_kl", {})[k] = a
        print(f"{k:>20}  {a:>10.3f}")

    # --- claim 2: creative vs non-creative on POS stats alone ---
    from sklearn.linear_model import LogisticRegression
    from sklearn.preprocessing import StandardScaler
    from sklearn.metrics import roc_auc_score

    # Rebuild with document groups so folds never share a source text.
    Xs, ys, gs = [], [], []
    from nltk.corpus import gutenberg, brown
    import re as _re
    group_id = 0
    for fid in ("austen-emma.txt", "melville-moby_dick.txt",
                "chesterton-brown.txt", "bryant-stories.txt"):
        for w in corpus._windows(gutenberg.raw(fid))[:40]:
            f = pos_features(w)
            if f:
                Xs.append(f["feat_vec"]); ys.append(1); gs.append(group_id)
        group_id += 1
    for cat in ("government", "learned"):
        text = " ".join(" ".join(s) for s in brown.sents(categories=cat))
        text = _re.sub(r"\s+([.,;:!?])", r"\1", text)
        for w in corpus._windows(text)[:80]:
            f = pos_features(w)
            if f:
                Xs.append(f["feat_vec"]); ys.append(0); gs.append(group_id)
        group_id += 1
    X, y, g = np.array(Xs), np.array(ys), np.array(gs)

    # Grouped folds with both classes present: test = one fiction source + one
    # noncreative source, neither seen in training. Fiction groups are 0-3,
    # noncreative groups 4-5 (order of construction above).
    aucs = []
    for fic_g, non_g in ((0, 4), (1, 5), (2, 4), (3, 5)):
        te = (g == fic_g) | (g == non_g)
        tr = ~te
        sc = StandardScaler().fit(X[tr])
        m = LogisticRegression(max_iter=2000).fit(sc.transform(X[tr]), y[tr])
        aucs.append(roc_auc_score(y[te], m.predict_proba(sc.transform(X[te]))[:, 1]))
    report["creative_vs_noncreative_cv_auc"] = [float(a) for a in aucs]
    print(f"\ncreative-vs-noncreative grouped-CV AUC: "
          f"{np.mean(aucs):.3f} (folds: {['%.2f' % a for a in aucs]})")

    # Which POS features carry the creative signature?
    sc = StandardScaler().fit(X)
    m = LogisticRegression(max_iter=2000).fit(sc.transform(X), y)
    names = TAGS + ["bigram_entropy", "content_ratio", "ttr", "mean_word_len"]
    coef = sorted(zip(names, m.coef_[0]), key=lambda t: -abs(t[1]))
    report["top_creative_features"] = [(n, float(w)) for n, w in coef[:8]]
    print("top features (+ = creative): " +
          ", ".join(f"{n} {w:+.2f}" for n, w in coef[:8]))

    # Where does the user's own writing land on the creative axis?
    if feats.get("real_user"):
        Xu = np.array([f["feat_vec"] for f in feats["real_user"]])
        pu = m.predict_proba(sc.transform(Xu))[:, 1]
        report["real_user_creative_score"] = {
            "mean": float(np.mean(pu)), "p10": float(np.percentile(pu, 10)),
            "p90": float(np.percentile(pu, 90)), "n": int(len(pu))}
        print(f"user windows on the creative axis: mean {np.mean(pu):.2f} "
              f"(p10 {np.percentile(pu, 10):.2f}, p90 {np.percentile(pu, 90):.2f})")

    with open(RESULTS / "pos_quality.json", "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"\nwrote {RESULTS / 'pos_quality.json'}")


if __name__ == "__main__":
    main()
