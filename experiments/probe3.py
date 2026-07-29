# probe3: honesty pass. Codex review said adjacent-chunk autocorrelation +
# selection-on-the-metric inflate the LOO numbers. Measure how much.
#   - blocked CV with a purge gap (neighbors excluded from training)
#   - nested alpha/layer selection (inner folds only)
#   - Spearman + MAE alongside Pearson, vs a train-mean baseline
#   - permutation test for a null distribution
import json, os
import numpy as np

DIR = os.path.dirname(os.path.abspath(__file__))
chunks = json.load(open(os.path.join(DIR, "chunks.json"), encoding="utf-8"))
sc = {}
for j in ("haiku", "sonnet", "fable"):
    for s in json.load(open(os.path.join(DIR, f"scores-{j}.json"), encoding="utf-8")):
        sc.setdefault(s["idx"], []).append(s["salience"])
assert all(len(v) == 3 for v in sc.values()), "expected exactly 3 ratings/chunk"
rows = [(c, float(np.mean(sc[c["idx"]]))) for c in chunks if c["idx"] in sc]
texts = [c["text"] for c, _ in rows]
y = np.array([t for _, t in rows])
n = len(y)
print(f"n={n}")

from sklearn.linear_model import Ridge
from sklearn.preprocessing import StandardScaler
from scipy.stats import spearmanr

ALPHAS = (1.0, 10.0, 100.0, 1000.0)

def blocked_folds(n, k=5, purge=1):
    """Contiguous blocks as test sets; purge `purge` neighbors from training."""
    edges = np.linspace(0, n, k + 1).astype(int)
    for i in range(k):
        te = np.arange(edges[i], edges[i + 1])
        lo, hi = te[0] - purge, te[-1] + purge
        tr = np.array([j for j in range(n) if j < lo or j > hi])
        yield tr, te

def fit_predict(Xtr, ytr, Xte, alpha):
    s = StandardScaler().fit(Xtr)
    return Ridge(alpha=alpha).fit(s.transform(Xtr), ytr).predict(s.transform(Xte))

def nested_blocked(X, y, k=5, purge=1):
    """Alpha chosen inside training folds only — no peeking at outer test."""
    preds = np.full(len(y), np.nan)
    for tr, te in blocked_folds(len(y), k, purge):
        best, best_err = None, np.inf
        for a in ALPHAS:
            errs = []
            for itr, ite in blocked_folds(len(tr), 4, purge):
                p = fit_predict(X[tr[itr]], y[tr[itr]], X[tr[ite]], a)
                errs.append(np.mean(np.abs(p - y[tr[ite]])))
            if np.mean(errs) < best_err:
                best_err, best = np.mean(errs), a
        preds[te] = fit_predict(X[tr], y[tr], X[te], best)
    return preds

def loo_leaky(X, y, alpha=100.0):
    preds = np.zeros(len(y))
    for i in range(len(y)):
        m = np.arange(len(y)) != i
        preds[i] = fit_predict(X[m], y[m], X[i:i+1], alpha)[0]
    return preds

def report(name, preds):
    r = float(np.corrcoef(preds, y)[0, 1])
    rho = float(spearmanr(preds, y).statistic)
    mae = float(np.mean(np.abs(preds - y)))
    base = float(np.mean(np.abs(y - np.mean(y))))  # predict-the-mean baseline
    print(f"{name:>34}: r={r:+.2f}  rho={rho:+.2f}  MAE={mae:.3f} (mean-baseline {base:.3f})")
    return r

# permutation null for the blocked protocol
def perm_null(X, y, iters=200, seed=0):
    rng = np.random.default_rng(seed)
    rs = []
    for _ in range(iters):
        yp = rng.permutation(y)
        p = nested_blocked(X, yp)
        ok = ~np.isnan(p)
        rs.append(float(np.corrcoef(p[ok], yp[ok])[0, 1]))
    return np.array(rs)

from sentence_transformers import SentenceTransformer
emb = SentenceTransformer("sentence-transformers/all-MiniLM-L6-v2")
X = np.array(emb.encode(texts, show_progress_bar=False))

print("\n--- MiniLM embeddings, protocol comparison ---")
report("LOO alpha=10 (original protocol)", loo_leaky(X, y, 10.0))
p_blocked = nested_blocked(X, y)
r_blocked = report("blocked CV + purge, nested alpha", p_blocked)
null = perm_null(X, y)
print(f"{'permutation null (blocked)':>34}: mean r={null.mean():+.2f}, "
      f"95th pct={np.percentile(null, 95):+.2f}, p(r>=obs)="
      f"{float(np.mean(null >= r_blocked)):.3f}")

# GPT-2: honest version — layer chosen inside training folds
import torch
from transformers import AutoTokenizer, AutoModel
tok = AutoTokenizer.from_pretrained("gpt2")
model = AutoModel.from_pretrained("gpt2", output_hidden_states=True)
model.eval()
with torch.no_grad():
    F = np.stack([
        torch.stack([h.mean(dim=1).squeeze(0)
                     for h in model(**tok(t, return_tensors="pt", truncation=True,
                                          max_length=512)).hidden_states]).numpy()
        for t in texts])
print("\n--- GPT-2 124M probe ---")
report("LOO, layer picked on all labels", loo_leaky(F[:, 1, :], y, 100.0))

# nested: pick layer AND alpha inside training folds only
preds = np.full(n, np.nan)
picked = []
for tr, te in blocked_folds(n, 5, 1):
    best, err_best = None, np.inf
    for L in range(F.shape[1]):
        for a in ALPHAS:
            errs = []
            for itr, ite in blocked_folds(len(tr), 4, 1):
                p = fit_predict(F[tr[itr]][:, L, :], y[tr[itr]], F[tr[ite]][:, L, :], a)
                errs.append(np.mean(np.abs(p - y[tr[ite]])))
            if np.mean(errs) < err_best:
                err_best, best = np.mean(errs), (L, a)
    picked.append(best[0])
    preds[te] = fit_predict(F[tr][:, best[0], :], y[tr], F[te][:, best[0], :], best[1])
report("blocked + nested layer/alpha", preds)
print(f"layers chosen per outer fold: {picked}")
