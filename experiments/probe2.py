# Optimized ladder: 2026-era sub-1GB models + method sweep, same 39 labels.
# Honesty note: sweeping configs on LOO r with n=39 inflates the winner, so we
# report the full sweep, not just the peak.
import json, os, gc
import numpy as np

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
print(f"n={len(rows)}")

def loo_ridge(X, y, alpha):
    from sklearn.linear_model import Ridge
    from sklearn.preprocessing import StandardScaler
    preds = np.zeros_like(y)
    for i in range(len(y)):
        m = np.arange(len(y)) != i
        sc = StandardScaler().fit(X[m])
        preds[i] = Ridge(alpha=alpha).fit(sc.transform(X[m]), y[m]).predict(
            sc.transform(X[i:i+1]))[0]
    return preds

def pearson(a, b):
    return float(np.corrcoef(a, b)[0, 1])

def sweep(name, X):
    best = (-2, None)
    for a in (1.0, 10.0, 100.0, 1000.0):
        r = pearson(loo_ridge(X, y, a), y)
        if r > best[0]:
            best = (r, a)
    preds = loo_ridge(X, y, best[1])
    print(f"{name:>44}: r={best[0]:+.2f} (alpha={best[1]:g}, r_flow={pearson(preds, flow):+.2f})")
    return best[0]

results = {}

# --- Qwen3 embedding 0.6B (2026 sub-1GB SOTA embedder) ---
try:
    from sentence_transformers import SentenceTransformer
    import torch
    emb = SentenceTransformer("Qwen/Qwen3-Embedding-0.6B",
                              model_kwargs={"torch_dtype": torch.float32},
                              tokenizer_kwargs={"padding_side": "left"})
    X = np.array(emb.encode(texts, show_progress_bar=False, batch_size=4))
    results["qwen3-emb"] = sweep("Qwen3-Embedding-0.6B (1024d)", X)
    del emb
    gc.collect()
except Exception as e:
    print(f"Qwen3-Embedding failed: {type(e).__name__}: {e}")

# --- Qwen3-0.6B base hidden states, layer/pooling sweep ---
try:
    import torch
    from transformers import AutoTokenizer, AutoModel
    tok = AutoTokenizer.from_pretrained("Qwen/Qwen3-0.6B-Base")
    model = AutoModel.from_pretrained("Qwen/Qwen3-0.6B-Base",
                                      output_hidden_states=True,
                                      torch_dtype=torch.float32,
                                      low_cpu_mem_usage=True)
    model.eval()
    feats = []  # (n, layers, 2, hidden): mean and last-token pooling
    with torch.no_grad():
        for t in texts:
            ids = tok(t, return_tensors="pt", truncation=True, max_length=512)
            out = model(**ids)
            per = []
            for h in out.hidden_states:
                per.append(torch.stack([h.mean(dim=1).squeeze(0),
                                        h[0, -1, :]]))
            feats.append(torch.stack(per).float().numpy())
    F = np.stack(feats)
    del model
    gc.collect()
    n_layers = F.shape[1]
    best = (-2, None)
    for L in range(0, n_layers, 2):
        for pi, pname in ((0, "mean"), (1, "last")):
            r = pearson(loo_ridge(F[:, L, pi, :], y, 100.0), y)
            if r > best[0]:
                best = (r, (L, pname))
            print(f"  qwen3-0.6b layer {L:2d} {pname:>4}: r={r:+.2f}")
    print(f"best Qwen3-0.6B probe: layer {best[1][0]} {best[1][1]} r={best[0]:+.2f}")
    results["qwen3-probe"] = best[0]
except Exception as e:
    print(f"Qwen3 base failed: {type(e).__name__}: {e}")

print("\nsummary:", {k: round(v, 2) for k, v in results.items()},
      "| prior: gpt2 probe +0.59, MiniLM +0.45, lexical +0.05, ceiling +0.91")
