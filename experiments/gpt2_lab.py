# Shared GPT-2 lab helpers for offline session audit and experiment scripts.
#
# Lab instrument only — never imported by the C++ plugin. Scores typed-stream
# windows with GPT-2 124M surprisal; prose band is calibrated on fiction
# (median/MAD) so junk labels never touch the threshold.
import math
from pathlib import Path

import numpy as np

import corpus

RESULTS = Path(__file__).parent / "results"
DATA = Path(__file__).parent / "data"


def gpt2_cached() -> bool:
    """True if GPT-2 124M weights+tokenizer are already in the local HF cache."""
    try:
        from transformers import GPT2LMHeadModel, GPT2TokenizerFast
        GPT2TokenizerFast.from_pretrained("gpt2", local_files_only=True)
        GPT2LMHeadModel.from_pretrained("gpt2", local_files_only=True)
        return True
    except Exception:
        return False


def ensure_nltk_band_data():
    """Fiction-band calibration needs Gutenberg (and corpus.py may use Brown)."""
    import nltk
    for pkg in ("gutenberg", "brown"):
        try:
            nltk.data.find(f"corpora/{pkg}")
        except LookupError:
            nltk.download(pkg, quiet=True)


def download_gpt2():
    """Fetch GPT-2 + NLTK band corpora into the local cache (explicit consent)."""
    from transformers import GPT2LMHeadModel, GPT2TokenizerFast
    GPT2TokenizerFast.from_pretrained("gpt2")
    GPT2LMHeadModel.from_pretrained("gpt2")
    ensure_nltk_band_data()


class Gpt2Scorer:
    """Per-token surprisal under GPT-2 124M causal head (bits/token)."""

    def __init__(self, local_files_only=True):
        # Default local-only: the plugin prompts before any network fetch.
        import torch
        from transformers import GPT2LMHeadModel, GPT2TokenizerFast
        self.torch = torch
        self.tok = GPT2TokenizerFast.from_pretrained(
            "gpt2", local_files_only=local_files_only)
        self.model = GPT2LMHeadModel.from_pretrained(
            "gpt2", local_files_only=local_files_only)
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
        s = -logp[torch.arange(tgt.shape[0]), tgt] / math.log(2)
        s = s.numpy()
        return dict(mean=float(np.mean(s)), std=float(np.std(s)),
                    p10=float(np.percentile(s, 10)),
                    p90=float(np.percentile(s, 90)),
                    n_tok=int(len(s)))


def fiction_band(scorer=None, limit_per_book=40):
    """Median/MAD of mean bits/token on Gutenberg fiction windows."""
    scorer = scorer or Gpt2Scorer()
    means = []
    for w in corpus.fiction_windows(limit_per_book=limit_per_book):
        s = scorer.surprisal_stats(w)
        if s:
            means.append(s["mean"])
    means = np.array(means, dtype=float)
    center = float(np.median(means))
    mad = float(np.median(np.abs(means - center))) or 1.0
    return {"center_bits": center, "mad_bits": mad, "n_windows": len(means)}


def band_distance(mean_bits, center, mad):
    return abs(mean_bits - center) / mad


def timed_inserts_from_raw_log(path):
    """Yield (t, text) for each insert event with captured text."""
    import re
    for line in open(path, encoding="utf-8", errors="replace"):
        if '"ev":"ins"' not in line:
            continue
        tm = re.search(r'"t":([0-9.]+)', line)
        xm = re.search(r'"text":"(.*)"\}\s*$', line)
        if not tm or not xm:
            continue
        s = xm.group(1)
        s = s.replace('\\"', '"').replace("\\\\", "\\")
        s = re.sub(r"\\u00([0-9a-fA-F]{2})",
                   lambda g: chr(int(g.group(1), 16)), s)
        yield float(tm.group(1)), s


def timed_windows_from_raw_log(path, n=corpus.WINDOW_CHARS, stride=corpus.STRIDE):
    """
    Sliding windows over the typed stream, each carrying the timestamp of the
    insert that completed it (seconds since session start as logged).
    """
    stream = []
    times = []  # per-char end time of the insert that wrote that char
    for t, text in timed_inserts_from_raw_log(path):
        for ch in text:
            stream.append(ch)
            times.append(t)
    full = "".join(stream)
    if len(full) < n // 2:
        return []
    out = []
    for i in range(0, max(1, len(full) - n + 1), stride):
        chunk = full[i:i + n]
        if len(chunk) < n // 2:
            continue
        end = i + len(chunk) - 1
        out.append({
            "start_char": i,
            "end_char": end,
            "t_end": times[end] if end < len(times) else times[-1],
            "text": chunk,
        })
    return out
