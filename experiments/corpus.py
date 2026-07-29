# Shared evaluation corpus for the quality/salience experiments.
#
# Windows of the *typed stream* (matching the plugin's ContentWindow: inserted
# chars appended in order, deletions ignored), from four sources:
#   real_user   — reconstructed from local raw session logs (never committed;
#                 this repo is public, the data dir is gitignored)
#   fiction     — NLTK Gutenberg novels (competent creative prose)
#   noncreative — NLTK Brown government/learned (competent, not creative)
#   junk_*      — synthesized junk classes matching how a human actually games
#                 a typing-rate reward: token loops, keyboard mash, stall
#                 filler, stopword streams, retyped sentences
#
# All generators are seeded: the corpus is reproducible without committing it.
import json
import os
import random
import re
from pathlib import Path

WINDOW_CHARS = 600   # matches ContentWindow max_chars in src/core/content.h
STRIDE = 300

RAW_LOG_DIR = Path(os.environ.get("APPDATA", "")) / "Notepad++/plugins/config/SkinnerBoxPP-logs"


def _windows(text, n=WINDOW_CHARS, stride=STRIDE):
    text = re.sub(r"\s+", " ", text).strip()
    return [text[i:i + n] for i in range(0, max(1, len(text) - n + 1), stride)
            if len(text[i:i + n]) >= n // 2]


def typed_stream_from_raw_log(path):
    """Concatenate ins-event text in typing order — what ContentWindow saw."""
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if '"ev":"ins"' not in line:
            continue
        m = re.search(r'"text":"(.*)"\}\s*$', line)
        if m:
            s = m.group(1)
            s = s.replace('\\"', '"').replace("\\\\", "\\")
            s = re.sub(r"\\u00([0-9a-fA-F]{2})", lambda g: chr(int(g.group(1), 16)), s)
            out.append(s)
    return "".join(out)


def real_user_windows():
    wins = []
    if RAW_LOG_DIR.is_dir():
        for p in sorted(RAW_LOG_DIR.glob("session-*.raw.jsonl")):
            stream = typed_stream_from_raw_log(p)
            if len(stream) >= 400:
                wins += _windows(stream)
    return wins


def fiction_windows(limit_per_book=40):
    from nltk.corpus import gutenberg
    wins = []
    for fid in ("austen-emma.txt", "melville-moby_dick.txt",
                "chesterton-brown.txt", "bryant-stories.txt"):
        wins += _windows(gutenberg.raw(fid))[:limit_per_book]
    return wins


def noncreative_windows(limit_per_cat=60):
    from nltk.corpus import brown
    wins = []
    for cat in ("government", "learned"):
        text = " ".join(" ".join(s) for s in brown.sents(categories=cat))
        text = re.sub(r"\s+([.,;:!?])", r"\1", text)
        wins += _windows(text)[:limit_per_cat]
    return wins


# --- junk generators: what a human actually types to spoof a momentum meter ---

_QWERTY = "asdfjkl;ghqwertyuiopzxcvbnm"
_STOPS = ("the and a to of in it is was that for on as with at by an be this "
          "have from or had not but what all were when we there can").split()
_FILL = ["uuuh", "umm", "hmm", "uhhh", "mmm", "erm", "uh", "um"]
_LOOP_SENTENCES = [
    "i am just typing this sentence again and again to keep the meter warm. ",
    "the quick brown fox jumps over the lazy dog every single time. ",
]


def junk_windows(n_per_class=60, seed=7):
    rng = random.Random(seed)
    classes = {}

    def gen(name, make):
        classes[name] = [make() for _ in range(n_per_class)]

    gen("repeat_token", lambda: (
        " ".join([rng.choice(["duper", "super", "really", "very", "nice"])] * 120)[:WINDOW_CHARS]))
    gen("keyboard_mash", lambda: (
        "".join(rng.choice(_QWERTY + "  ") for _ in range(WINDOW_CHARS))))
    gen("stall_filler", lambda: (
        " ".join(rng.choice(_FILL) if rng.random() < 0.7 else rng.choice(_STOPS)
                 for _ in range(140))[:WINDOW_CHARS]))
    gen("stopword_stream", lambda: (
        " ".join(rng.choice(_STOPS) for _ in range(130))[:WINDOW_CHARS]))
    gen("loop_sentence", lambda: (
        (rng.choice(_LOOP_SENTENCES) * 12)[:WINDOW_CHARS]))
    return classes


def build(save_dir=None):
    corpus = {
        "real_user": real_user_windows(),
        "fiction": fiction_windows(),
        "noncreative": noncreative_windows(),
    }
    corpus.update({f"junk_{k}": v for k, v in junk_windows().items()})
    if save_dir:
        Path(save_dir).mkdir(parents=True, exist_ok=True)
        with open(Path(save_dir) / "corpus.json", "w", encoding="utf-8") as f:
            json.dump(corpus, f)
    return corpus


if __name__ == "__main__":
    c = build(save_dir=Path(__file__).parent / "data")
    for k, v in c.items():
        print(f"{k:>20}: {len(v)} windows")
