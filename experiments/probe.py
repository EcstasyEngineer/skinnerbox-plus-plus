# Ladder experiment: lexical vs embeddings vs baby-transformer hidden-state
# probe, all predicting mean judge salience on the 39 labeled session chunks.
# Leave-one-out CV everywhere (n is tiny; this is feasibility, not validation).
import json, math, os, sys
import numpy as np
from collections import Counter

DIR = os.path.dirname(os.path.abspath(__file__))

chunks = json.load(open(os.path.join(DIR, "chunks.json"), encoding="utf-8"))
scores = {}
for j in ("haiku", "sonnet", "fable"):
    for s in json.load(open(os.path.join(DIR, f"scores-{j}.json"), encoding="utf-8")):
        scores.setdefault(s["idx"], []).append(s["salience"])

rows = [(c, np.mean(scores[c["idx"]])) for c in chunks if c["idx"] in scores]
texts = [c["text"] for c, _ in rows]
y = np.array([t for _, t in rows])
flow = np.array([c["flow"] for c, _ in rows])
print(f"n={len(rows)}, salience mean={y.mean():.2f} sd={y.std():.2f}")

def loo_ridge(X, y, alpha=1.0):
    from sklearn.linear_model import Ridge
    from sklearn.preprocessing import StandardScaler
    preds = np.zeros_like(y)
    for i in range(len(y)):
        mask = np.arange(len(y)) != i
        sc = StandardScaler().fit(X[mask])
        m = Ridge(alpha=alpha).fit(sc.transform(X[mask]), y[mask])
        preds[i] = m.predict(sc.transform(X[i:i+1]))[0]
    return preds

def pearson(a, b):
    return float(np.corrcoef(a, b)[0, 1])

def report(name, preds):
    print(f"{name:>28}: r(pred, salience)={pearson(preds, y):+.2f}   "
          f"r(pred, flow)={pearson(preds, flow):+.2f}")

# --- rung 1: lexical ---
def lexical_feats(t):
    words = t.lower().split()
    toks = [w.strip('.,!?()"\'') for w in words if w]
    c = Counter(toks)
    rep = 1 - len(c) / len(toks) if toks else 0          # repeated-token mass
    top = c.most_common(1)[0][1] / len(toks) if toks else 0
    ent = 0.0
    n = len(t)
    if n:
        for ch, k in Counter(t).items():
            p = k / n
            ent -= p * math.log2(p)
    stall = sum(1 for w in toks if set(w) <= {"u", "h"} and len(w) > 2)
    mwl = np.mean([len(w) for w in toks]) if toks else 0
    return [rep, top, ent, stall / max(1, len(toks)), mwl, len(toks)]

X_lex = np.array([lexical_feats(t) for t in texts])
report("lexical (6 feats)", loo_ridge(X_lex, y))

# --- rung 2: sentence embeddings ---
from sentence_transformers import SentenceTransformer
emb = SentenceTransformer("sentence-transformers/all-MiniLM-L6-v2")
X_emb = np.array(emb.encode(texts, show_progress_bar=False))
report("MiniLM embeddings (384d)", loo_ridge(X_emb, y, alpha=10.0))

# --- rung 3: GPT-2 hidden-state probe, per layer ---
import torch
from transformers import AutoTokenizer, AutoModel
tok = AutoTokenizer.from_pretrained("gpt2")
model = AutoModel.from_pretrained("gpt2", output_hidden_states=True)
model.eval()

layer_feats = None
with torch.no_grad():
    all_layers = []
    for t in texts:
        ids = tok(t, return_tensors="pt", truncation=True, max_length=512)
        out = model(**ids)
        # mean-pool tokens per layer -> (n_layers, hidden)
        pooled = torch.stack([h.mean(dim=1).squeeze(0) for h in out.hidden_states])
        all_layers.append(pooled.numpy())
    layer_feats = np.stack(all_layers)  # (n, layers, hidden)

best = (None, -2)
print("\nper-layer GPT-2 probe:")
for L in range(layer_feats.shape[1]):
    preds = loo_ridge(layer_feats[:, L, :], y, alpha=100.0)
    r = pearson(preds, y)
    marker = ""
    if r > best[1]:
        best = (L, r)
        marker = " <- best so far"
    print(f"  layer {L:2d}: r={r:+.2f}{marker}")
preds_best = loo_ridge(layer_feats[:, best[0], :], y, alpha=100.0)
report(f"GPT-2 probe (layer {best[0]})", preds_best)

# --- combined: lexical + best hidden layer ---
X_comb = np.concatenate([X_lex, layer_feats[:, best[0], :]], axis=1)
report("lexical + GPT-2 combined", loo_ridge(X_comb, y, alpha=100.0))

# ceiling reference: single-judge vs mean-of-others
h = {s["idx"]: s["salience"] for s in json.load(open(os.path.join(DIR, "scores-haiku.json"), encoding="utf-8"))}
oth = {}
for j in ("sonnet", "fable"):
    for s in json.load(open(os.path.join(DIR, f"scores-{j}.json"), encoding="utf-8")):
        oth.setdefault(s["idx"], []).append(s["salience"])
common = [c["idx"] for c, _ in rows]
print(f"\nreference ceiling: haiku vs mean(sonnet,fable) r="
      f"{pearson(np.array([h[i] for i in common]), np.array([np.mean(oth[i]) for i in common])):+.2f}")
