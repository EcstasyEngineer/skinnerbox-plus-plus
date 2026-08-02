# Offline GPT-2 session audit pipeline.
#
# Design law (docs/architecture.md): quality ground truth is EXTERNAL and
# POST-SESSION. This script never asks the writer anything. It:
#
#   1. Reconstructs typed-stream windows from an opt-in raw session log
#   2. Scores each window with GPT-2 surprisal + the runtime lexical facets
#   3. Writes an audit packet the external auditor fills in offline
#   4. Correlates filled labels against GPT-2 / facets (when labels exist)
#
# Usage:
#   python audit_session.py prepare path\to\session-....raw.jsonl
#   python audit_session.py correlate path\to\packet.json
#
# External label file (same stem as packet, suffix .labels.jsonl), one object
# per line, id must match a window:
#   {"id":"w0003","quality":0.72,"energy":0.65,"on_topic":0.80,"junk":0}
#
# Scales are 0–1 continuous except junk (0/1). Omit any field you skip.
import argparse
import json
import math
import time
from collections import Counter
from pathlib import Path

import numpy as np

import corpus
import gpt2_lab

DATA = Path(__file__).parent / "data"
AUDIT = DATA / "audit"


# --- lexical facets (mirror of junk_gate / content.cpp) ---------------------

def lexical_facets(text):
    hist = Counter(text)
    n = len(text)
    ent = -sum((k / n) * math.log2(k / n) for k in hist.values()) if n else 0.0
    toks = [t.lower() for t in "".join(
        c if c.isalpha() else " " for c in text).split()]
    rep = 1.0 - len(set(toks)) / len(toks) if toks else 0.0
    stall = sum(1 for t in toks if len(t) >= 3 and set(t) <= set("uhm"))
    stall_frac = stall / len(toks) if toks else 0.0
    return dict(entropy=ent, repetition=rep, stall_frac=stall_frac,
                window_chars=n)


def prepare(raw_path: Path, out_dir: Path | None = None):
    raw_path = raw_path.resolve()
    if not raw_path.is_file():
        raise SystemExit(f"raw log not found: {raw_path}")

    out_dir = out_dir or (AUDIT / raw_path.stem)
    out_dir.mkdir(parents=True, exist_ok=True)
    packet_path = out_dir / "packet.json"
    labels_path = out_dir / "labels.jsonl"
    template_path = out_dir / "labels.TEMPLATE.jsonl"

    print(f"windowing {raw_path.name}...")
    windows = gpt2_lab.timed_windows_from_raw_log(raw_path)
    if not windows:
        raise SystemExit("no windows (need ~300+ typed chars with text capture)")

    print(f"{len(windows)} windows; loading GPT-2 124M...")
    scorer = gpt2_lab.Gpt2Scorer()
    print("calibrating fiction prose band...")
    band = gpt2_lab.fiction_band(scorer)
    center, mad = band["center_bits"], band["mad_bits"]
    print(f"  band center {center:.2f} bits/tok  MAD {mad:.2f}  "
          f"(n={band['n_windows']})")

    scored = []
    t0 = time.perf_counter()
    for i, w in enumerate(windows):
        stats = scorer.surprisal_stats(w["text"])
        facets = lexical_facets(w["text"])
        row = {
            "id": f"w{i:04d}",
            "start_char": w["start_char"],
            "end_char": w["end_char"],
            "t_end": w["t_end"],
            "text": w["text"],
            "facets": facets,
            "gpt2": None,
        }
        if stats:
            dist = gpt2_lab.band_distance(stats["mean"], center, mad)
            row["gpt2"] = {
                **stats,
                "band_dist": dist,
                "band_center": center,
                "band_mad": mad,
            }
        scored.append(row)
        if (i + 1) % 5 == 0 or i + 1 == len(windows):
            print(f"  scored {i + 1}/{len(windows)}")
    ms = 1000.0 * (time.perf_counter() - t0) / max(1, len(windows))

    packet = {
        "schema": "sbpp.audit_packet.v1",
        "source_raw": str(raw_path),
        "window_chars": corpus.WINDOW_CHARS,
        "stride": corpus.STRIDE,
        "model": "gpt2",
        "band": band,
        "ms_per_window": ms,
        "n_windows": len(scored),
        "label_fields": {
            "quality": "0-1 overall writing quality for this window (external)",
            "energy": "0-1 alive / generative energy (external)",
            "on_topic": "0-1 still on the session's apparent thread (external)",
            "junk": "0 or 1 — gaming/slop/filler, not prose (external)",
        },
        "windows": scored,
    }
    with open(packet_path, "w", encoding="utf-8") as f:
        json.dump(packet, f, ensure_ascii=False, indent=2)

    # Template for the external auditor — ids only, empty slots. No writer UX.
    with open(template_path, "w", encoding="utf-8") as f:
        for row in scored:
            f.write(json.dumps({
                "id": row["id"],
                "quality": None,
                "energy": None,
                "on_topic": None,
                "junk": None,
            }, ensure_ascii=False) + "\n")

    if not labels_path.exists():
        # Starter empty labels file the auditor renames/fills; leave template
        # as the copy-paste source so a partial fill doesn't get clobbered.
        labels_path.write_text("", encoding="utf-8")

    print(f"\nwrote {packet_path}")
    print(f"      {template_path}")
    print(f"fill  {labels_path}  (copy lines from the template; external only)")
    print(f"then: python audit_session.py correlate {packet_path}")
    return packet_path


def load_labels(path: Path):
    by_id = {}
    if not path.is_file() or path.stat().st_size == 0:
        return by_id
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        obj = json.loads(line)
        if obj.get("id"):
            by_id[obj["id"]] = obj
    return by_id


def _pearson(xs, ys):
    if len(xs) < 3:
        return None
    return float(np.corrcoef(xs, ys)[0, 1])


def correlate(packet_path: Path, labels_path: Path | None = None):
    packet_path = packet_path.resolve()
    packet = json.load(open(packet_path, encoding="utf-8"))
    labels_path = labels_path or packet_path.with_name("labels.jsonl")
    labels = load_labels(labels_path)
    if not labels:
        raise SystemExit(
            f"no labels in {labels_path}\n"
            f"copy {packet_path.with_name('labels.TEMPLATE.jsonl')} → "
            f"labels.jsonl and fill quality/energy/on_topic/junk externally")

    fields = ("quality", "energy", "on_topic", "junk")
    predictors = {
        "gpt2_mean_bits": lambda w: (w.get("gpt2") or {}).get("mean"),
        "gpt2_band_dist": lambda w: (w.get("gpt2") or {}).get("band_dist"),
        "facet_entropy": lambda w: w["facets"]["entropy"],
        "facet_repetition": lambda w: w["facets"]["repetition"],
        "facet_stall": lambda w: w["facets"]["stall_frac"],
    }

    joined = []
    for w in packet["windows"]:
        lab = labels.get(w["id"])
        if not lab:
            continue
        # skip rows where every label slot is still null
        if all(lab.get(f) is None for f in fields):
            continue
        joined.append((w, lab))

    if not joined:
        raise SystemExit(
            f"{len(labels)} label rows but none filled — need at least one "
            f"non-null quality/energy/on_topic/junk")

    report = {
        "packet": str(packet_path),
        "labels": str(labels_path),
        "n_labeled": len(joined),
        "n_windows": packet["n_windows"],
        "correlations": {},
    }
    print(f"{len(joined)}/{packet['n_windows']} windows labeled\n")
    print(f"{'label':>10}  {'predictor':>16}  {'r':>7}  n")
    for field in fields:
        for pname, pfun in predictors.items():
            xs, ys = [], []
            for w, lab in joined:
                y = lab.get(field)
                x = pfun(w)
                if y is None or x is None:
                    continue
                try:
                    y = float(y)
                except (TypeError, ValueError):
                    continue
                xs.append(x)
                ys.append(y)
            r = _pearson(xs, ys)
            report["correlations"].setdefault(field, {})[pname] = {
                "r": r, "n": len(xs),
            }
            r_s = f"{r:+.3f}" if r is not None else "   n/a"
            print(f"{field:>10}  {pname:>16}  {r_s:>7}  {len(xs)}")

    out = packet_path.with_name("correlate.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"\nwrote {out}")
    return out


def main():
    ap = argparse.ArgumentParser(
        description="Offline GPT-2 session audit (external labels only)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_prep = sub.add_parser(
        "prepare",
        help="window a raw log, score with GPT-2, write audit packet")
    p_prep.add_argument("raw", type=Path, help="session-*.raw.jsonl path")
    p_prep.add_argument("-o", "--out", type=Path, default=None,
                        help="output directory (default: data/audit/<stem>)")

    p_corr = sub.add_parser(
        "correlate",
        help="correlate external labels against GPT-2 / facets")
    p_corr.add_argument("packet", type=Path, help="packet.json from prepare")
    p_corr.add_argument("--labels", type=Path, default=None,
                        help="labels.jsonl (default: next to packet)")

    args = ap.parse_args()
    if args.cmd == "prepare":
        prepare(args.raw, args.out)
    elif args.cmd == "correlate":
        correlate(args.packet, args.labels)


if __name__ == "__main__":
    main()
