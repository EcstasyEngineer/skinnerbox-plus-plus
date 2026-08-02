# Long-lived GPT-2 lab host for SkinnerBox++ advanced_debug.
#
# Protocol (stdin/stdout, one JSON object per line, -u unbuffered):
#   → {"op":"check"}
#   ← {"ok":true,"present":true|false}
#   → {"op":"download"}          # only after the user clicks Yes in the plugin
#   ← {"ok":true,"present":true}
#   → {"op":"hello"}             # loads from local cache only (no silent net)
#   ← {"ok":true,"ready":true,"center_bits":...,"mad_bits":...}
#   → {"op":"score","text":"..."}
#   ← {"ok":true,"mean":...,"std":...,"band_dist":...,"n_tok":...,"ms":...}
#   → {"op":"quit"}
#
# Also CLI:  python gpt2_lab_host.py --check | --download
#
# Same math as gpt2_lab.py / junk_gate.py. CPU only. Weights are NEVER shipped
# with the plugin — download is opt-in and cached by Hugging Face locally.
import json
import sys
import time

import gpt2_lab


def reply(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def op_check():
    present = gpt2_lab.gpt2_cached()
    reply({"ok": True, "present": present, "model": "gpt2"})
    return present


def op_download():
    try:
        gpt2_lab.download_gpt2()
        reply({"ok": True, "present": True, "model": "gpt2"})
        return True
    except Exception as e:
        reply({"ok": False, "present": False, "error": str(e)})
        return False


def main_cli(argv):
    if "--check" in argv:
        ok = op_check()
        return 0 if ok else 2
    if "--download" in argv:
        return 0 if op_download() else 1
    return None


def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass

    cli = main_cli(sys.argv[1:])
    if cli is not None:
        sys.exit(cli)

    scorer = None
    band = None

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError as e:
            reply({"ok": False, "error": f"bad json: {e}"})
            continue
        op = msg.get("op")
        if op == "quit":
            reply({"ok": True, "bye": True})
            return
        if op == "check":
            op_check()
            continue
        if op == "download":
            op_download()
            continue
        if op == "hello":
            try:
                if not gpt2_lab.gpt2_cached():
                    reply({
                        "ok": False,
                        "ready": False,
                        "error": "model not cached; run download after user consent",
                    })
                    continue
                if scorer is None:
                    gpt2_lab.ensure_nltk_band_data()
                    scorer = gpt2_lab.Gpt2Scorer(local_files_only=True)
                    band = gpt2_lab.fiction_band(scorer)
                reply({
                    "ok": True,
                    "ready": True,
                    "center_bits": band["center_bits"],
                    "mad_bits": band["mad_bits"],
                    "model": "gpt2",
                })
            except Exception as e:
                reply({"ok": False, "ready": False, "error": str(e)})
            continue
        if op == "score":
            if scorer is None or band is None:
                reply({"ok": False, "error": "host not ready; send hello first"})
                continue
            text = msg.get("text") or ""
            if len(text) < 40:
                reply({"ok": False, "error": "text too short"})
                continue
            t0 = time.perf_counter()
            try:
                stats = scorer.surprisal_stats(text)
                ms = 1000.0 * (time.perf_counter() - t0)
                if not stats:
                    reply({"ok": False, "error": "too few tokens", "ms": ms})
                    continue
                dist = gpt2_lab.band_distance(
                    stats["mean"], band["center_bits"], band["mad_bits"])
                reply({
                    "ok": True,
                    "mean": stats["mean"],
                    "std": stats["std"],
                    "p10": stats["p10"],
                    "p90": stats["p90"],
                    "n_tok": stats["n_tok"],
                    "band_dist": dist,
                    "ms": ms,
                })
            except Exception as e:
                reply({"ok": False, "error": str(e)})
            continue
        reply({"ok": False, "error": f"unknown op: {op}"})


if __name__ == "__main__":
    main()
