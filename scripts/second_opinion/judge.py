"""Judge calibration (spec §4.2). Reuses synthetic_judge.py unchanged against
local Gemma, records with score.py --rater synthetic (never the human key),
and measures agreement with Seth's own ratings on the same items."""
import csv
import glob
import json
import os
import subprocess
import sys

from . import stats, store

SCRIPTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLIND_AB = os.path.join(SCRIPTS, "blind_ab")


def _key_is_detection_mode(key_path):
    """False for a missing/unparseable key, or a key stamped with a "_mode"
    other than "detection" (e.g. a blind-A/B preference-mode run, whose key
    means the MODEL's side rather than the human's answer)."""
    try:
        with open(key_path) as f:
            key = json.load(f)
    except (OSError, ValueError):
        return False
    mode = key.get("_mode") if isinstance(key, dict) else None
    return mode is None or mode == "detection"


def latest_run_dir(root):
    if not os.path.isdir(root):
        return None
    dirs = [d for d in glob.glob(os.path.join(root, "*"))
            if os.path.isfile(os.path.join(d, "rating_sheet.csv"))
            and os.path.isfile(os.path.join(d, "answer_key.json"))
            and _key_is_detection_mode(os.path.join(d, "answer_key.json"))]
    return max(dirs, key=os.path.getmtime) if dirs else None


def _choices(path):
    out = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            ch = (r.get("choice") or "").strip().upper()
            if ch in ("A", "B") and r.get("id"):
                out[r["id"]] = ch
    return out


def _is_judged(path):
    with open(path, newline="") as f:
        return any(store.row_is_judged(r) for r in csv.DictReader(f))


def human_choices(run_dir):
    out = {}
    for p in sorted(glob.glob(os.path.join(run_dir, "*.csv"))):
        if not _is_judged(p):
            out.update(_choices(p))
    return out


def calibration(human, judged):
    shared = sorted(set(human) & set(judged))
    pairs = [(human[i], judged[i]) for i in shared]
    if len(pairs) >= 20:
        agree = round(sum(1 for a, b in pairs if a == b) / len(pairs), 4)
        kappa = stats.cohen_kappa(pairs)
        kappa = round(kappa, 4) if isinstance(kappa, float) else kappa
    else:
        agree = stats.NOT_MEASURED
        kappa = stats.NOT_MEASURED
    return {"shared": len(pairs), "agreement": agree, "kappa": kappa}


def judge_pass(backend, run_dir, out_dir, run=subprocess.run, timeout=None):
    """timeout (seconds, or None for no limit) is forwarded to every subprocess.run
    call so the judge job honours the nightly runner's --deadline: a run that
    outlives it raises subprocess.TimeoutExpired rather than keeping the local
    server resident indefinitely."""
    if not hasattr(backend, "base_url"):
        return {"skipped": "backend"}
    os.makedirs(out_dir, exist_ok=True)
    judged = os.path.join(out_dir, "judged.csv")
    results = os.path.join(out_dir, "judge-results.json")
    r1 = run([sys.executable, os.path.join(BLIND_AB, "synthetic_judge.py"),
              os.path.join(run_dir, "rating_sheet.csv"), "--out", judged,
              "--endpoint", backend.base_url + "/v1/chat/completions", "--model", backend.model],
             capture_output=True, text=True, timeout=timeout)
    if r1.returncode != 0:
        raise RuntimeError("synthetic_judge.py failed")
    r2 = run([sys.executable, os.path.join(BLIND_AB, "score.py"), judged,
              "--key", os.path.join(run_dir, "answer_key.json"), "--rater", "synthetic",
              "--json-out", results], capture_output=True, text=True, timeout=timeout)
    if r2.returncode != 0:
        raise RuntimeError("score.py failed")
    return {"results": json.load(open(results)),
            "calibration": calibration(human_choices(run_dir), _choices(judged))}
