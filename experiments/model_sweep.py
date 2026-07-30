# Experiment 04 — modern tiny causal LMs vs GPT-2, done honestly this time.
#
# The deleted probe2.py compared 2026 sub-1GB models by regressing pooled
# hidden states onto 39 LLM-judge labels with leaky LOO CV — the ranking it
# produced was methodology noise. This sweep re-asks the question with the
# deployment-shaped, label-free protocol from experiment 02, per model:
#
#   1. Junk band: per-token surprisal, prose band calibrated on fiction only
#      (median/MAD), AUC per junk class vs real windows. Does a modern tiny
#      model separate the classes GPT-2 was soft on (stall_filler 0.86,
#      stopword_stream 0.78)?
#   2. Register separation (the "stay on topic" use case): mean-pooled
#      MID-layer hidden states (fixed rule for every model — no layer
#      selection, that was probe3's lesson), nearest-centroid classification
#      of fiction / legal-academic / user-rambling with leave-one-source-out
#      CV (a source = a book / a Brown category / a session; no source ever
#      straddles train and test).
#   3. CPU latency per 600-char window (the deployment constraint).
#
# One forward pass per window yields both logits (surprisal) and hidden
# states (register features). Models load one at a time to fit laptop RAM.
import gc
import json
import math
import time
from pathlib import Path

import numpy as np

import corpus

DATA = Path(__file__).parent / "data"
RESULTS = Path(__file__).parent / "results"

MODELS = [
    "gpt2",                          # 124M, 2019 — the baseline
    "HuggingFaceTB/SmolLM2-135M",    # 135M, 2024 — the direct size peer
    "Qwen/Qwen3-0.6B-Base",          # 596M, 2025 — the sub-1GB class
]

N_JUNK_PER_CLASS = 30
N_FICTION = 25   # per book (4 books)
N_NONCREATIVE = 40  # per Brown category (2 categories)


def auc(pos, neg):
    from sklearn.metrics import roc_auc_score
    return float(roc_auc_score([1] * len(pos) + [0] * len(neg),
                               list(pos) + list(neg)))


def build_register_sets():
    """(text, register, source_group) triplets; groups never straddle folds."""
    import re
    from nltk.corpus import gutenberg, brown
    items = []
    for fid in ("austen-emma.txt", "melville-moby_dick.txt",
                "chesterton-brown.txt", "bryant-stories.txt"):
        for w in corpus._windows(gutenberg.raw(fid))[:N_FICTION]:
            items.append((w, "fiction", f"book:{fid}"))
    for cat in ("government", "learned"):
        text = " ".join(" ".join(s) for s in brown.sents(categories=cat))
        text = re.sub(r"\s+([.,;:!?])", r"\1", text)
        for w in corpus._windows(text)[:N_NONCREATIVE]:
            items.append((w, "legal_academic", f"brown:{cat}"))
    if corpus.RAW_LOG_DIR.is_dir():
        for p in sorted(corpus.RAW_LOG_DIR.glob("session-*.raw.jsonl")):
            stream = corpus.typed_stream_from_raw_log(p)
            if len(stream) >= 400:
                for w in corpus._windows(stream):
                    items.append((w, "rambling", f"session:{p.name}"))
    return items


class CausalScorer:
    def __init__(self, name):
        import torch
        from transformers import AutoModelForCausalLM, AutoTokenizer
        self.torch = torch
        self.tok = AutoTokenizer.from_pretrained(name)
        self.model = AutoModelForCausalLM.from_pretrained(
            name, torch_dtype=torch.float32, output_hidden_states=True)
        self.model.eval()

    def score(self, text):
        """-> (surprisal stats dict, mid-layer mean-pooled hidden state)."""
        torch = self.torch
        ids = self.tok(text, return_tensors="pt", truncation=True,
                       max_length=256).input_ids
        if ids.shape[1] < 8:
            return None, None
        with torch.no_grad():
            out = self.model(ids)
        logp = torch.log_softmax(out.logits[0, :-1].float(), dim=-1)
        tgt = ids[0, 1:]
        s = (-logp[torch.arange(tgt.shape[0]), tgt] / math.log(2)).numpy()
        mid = len(out.hidden_states) // 2
        h = out.hidden_states[mid][0].float().mean(dim=0).numpy()
        return dict(mean=float(np.mean(s))), h

    def free(self):
        del self.model
        gc.collect()


def logo_centroid_accuracy(X, labels, groups):
    """Leave-one-group-out nearest-centroid accuracy + per-class recall."""
    X = np.array(X)
    labels = np.array(labels)
    groups = np.array(groups)
    correct = {}
    total = {}
    for g in sorted(set(groups)):
        te = groups == g
        tr = ~te
        # A held-out group's own class must still exist in training.
        if len(set(labels[tr])) < len(set(labels)):
            continue
        cents = {c: X[tr & (labels == c)].mean(axis=0)
                 for c in sorted(set(labels[tr]))}
        for x, y in zip(X[te], labels[te]):
            pred = min(cents, key=lambda c: np.linalg.norm(x - cents[c]))
            total[y] = total.get(y, 0) + 1
            correct[y] = correct.get(y, 0) + (pred == y)
    recall = {c: correct.get(c, 0) / total[c] for c in total}
    overall = sum(correct.values()) / max(1, sum(total.values()))
    return overall, recall


def main():
    RESULTS.mkdir(exist_ok=True)
    cfile = DATA / "corpus.json"
    c = (json.load(open(cfile, encoding="utf-8")) if cfile.exists()
         else corpus.build(save_dir=DATA))
    junk_keys = sorted(k for k in c if k.startswith("junk_"))
    real = {"fiction": c["fiction"][:60], "real_user": c["real_user"]}
    reg_items = build_register_sets()
    print(f"register windows: {len(reg_items)} from "
          f"{len(set(g for _, _, g in reg_items))} sources")

    report = {}
    for name in MODELS:
        print(f"\n================ {name}")
        try:
            scorer = CausalScorer(name)
        except Exception as e:
            print(f"  SKIPPED (load failed): {e}")
            report[name] = {"error": str(e)[:200]}
            continue

        # --- junk band ---
        t0 = time.perf_counter()
        n_scored = 0

        def means(texts):
            nonlocal n_scored
            out = []
            for t in texts:
                s, _ = scorer.score(t)
                if s:
                    out.append(s["mean"])
                    n_scored += 1
            return out

        fic = means(real["fiction"])
        usr = means(real["real_user"])
        center = float(np.median(fic))
        mad = float(np.median(np.abs(np.array(fic) - center))) or 1.0
        real_band = [abs(m - center) / mad for m in fic + usr]
        m = {}
        for k in junk_keys:
            jb = [abs(x - center) / mad
                  for x in means(c[k][:N_JUNK_PER_CLASS])]
            m[k] = auc(jb, real_band)
        ms = 1000.0 * (time.perf_counter() - t0) / max(1, n_scored)
        print(f"  band center {center:.2f} bits/tok (MAD {mad:.2f}), "
              f"{ms:.0f} ms/window")
        for k, a in m.items():
            print(f"  {k:>22}: AUC {a:.3f}")

        # --- register separation ---
        X, ys, gs = [], [], []
        for w, y, g in reg_items:
            _, h = scorer.score(w)
            if h is not None:
                X.append(h)
                ys.append(y)
                gs.append(g)
        overall, recall = logo_centroid_accuracy(X, ys, gs)
        print(f"  register LOGO centroid acc: {overall:.3f}  " +
              "  ".join(f"{k} {v:.2f}" for k, v in sorted(recall.items())))

        report[name] = {"band_center_bits": center, "band_mad": mad,
                        "ms_per_window": ms, "junk_auc": m,
                        "register_overall_acc": overall,
                        "register_recall": recall}
        scorer.free()

    with open(RESULTS / "model_sweep.json", "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"\nwrote {RESULTS / 'model_sweep.json'}")


if __name__ == "__main__":
    main()
