#!/usr/bin/env python3
"""Task 11 — classifier-tier eval beside the human blind A/B (Binoculars AUC).

Builds the {trials:[{real_seth, ai_response, context}]} file binoculars_score.py
expects from a rated cycle directory (rating_sheet.csv + answer_key.json: the
key names the option that WAS real), runs the scorer with the SERVED base +
adapter, and writes ~/.human/logs/classifier-gate-<date>.json.

Refuses, writing nothing, when:
  * fewer than --min-n trials (default 20) — an AUC on 8 pairs is noise;
  * the production server on :8741 is up and --in-window was not passed —
    the scorer loads its own 56 GB model pair; never two loaders.

    classifier_gate.py --cycle-dir ~/.human/blind_ab_human/rated-<x> [--in-window]
"""
import argparse, csv, datetime, json, os, subprocess, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

def build_trials(cycle_dir):
    key = json.load(open(os.path.join(cycle_dir, "answer_key.json")))
    trials = []
    with open(os.path.join(cycle_dir, "rating_sheet.csv"), newline="") as f:
        for row in csv.DictReader(f):
            k = key.get(row["id"])
            if k not in ("A", "B"):
                continue
            real = row["option_A"] if k == "A" else row["option_B"]
            ai = row["option_B"] if k == "A" else row["option_A"]
            if not real.strip() or not ai.strip():
                continue
            trials.append({"i": row["id"], "context": row.get("context", ""), "real_seth": real, "ai_response": ai})
    return trials

def serving_up(url="http://127.0.0.1:8741/health"):
    try:
        urllib.request.urlopen(url, timeout=3); return True
    except Exception:
        return False

def served_adapter():
    try:
        c = json.load(open(os.path.expanduser("~/.human/config.json")))
        return c.get("personalization", {}).get("lora_adapter_path") or c.get("mlx_local", {}).get("adapter_path")
    except Exception:
        return None

def extract_auc(report):
    """binocular_score.py's real report nests scores under report["analysis"]["scores"]
    (top-level keys: base, adapter, contexted, n_texts, analysis, results). Prefer that
    shape; fall back to a legacy flat {scores: {...}} shape if analysis is absent.
    Returns (key, auc) or (None, None)."""
    scores = ((report.get("analysis") or {}).get("scores")
              or report.get("scores") or {})
    best = (None, None)
    for k, v in scores.items():
        if isinstance(v, dict) and "auc_oriented" in v:
            if best[1] is None or v["auc_oriented"] > best[1]:
                best = (k, v["auc_oriented"])
    return best

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycle-dir", required=True)
    ap.add_argument("--base", default="mlx-community/GLM-4.5-Air-4bit")
    ap.add_argument("--adapter", default=None)
    ap.add_argument("--min-n", type=int, default=20)
    ap.add_argument("--in-window", action="store_true", help="assert the caller stopped :8741 (never two loaders)")
    ap.add_argument("--out-dir", default=os.path.expanduser("~/.human/logs"))
    ap.add_argument("--trials-only", action="store_true", help="build the trials file and stop (no model)")
    a = ap.parse_args()
    trials = build_trials(a.cycle_dir)
    if len(trials) < a.min_n:
        sys.exit(f"REFUSING: {len(trials)} trials < {a.min_n} — an AUC on that few pairs is noise; nothing written")
    trials_path = os.path.join(a.cycle_dir, "classifier_trials.json")
    json.dump({"trials": trials}, open(trials_path, "w"))
    print(f"[classifier-gate] {len(trials)} trials -> {trials_path}")
    if a.trials_only:
        return 0
    if serving_up() and not a.in_window:
        sys.exit("REFUSING: :8741 is serving and --in-window not given — the scorer loads its own model pair; "
                 "run inside the retrain window (server stopped) or pass --in-window from a caller that owns it")
    adapter = a.adapter or served_adapter()
    if not adapter:
        sys.exit("REFUSING: no adapter (pass --adapter or set personalization.lora_adapter_path)")
    stamp = datetime.date.today().isoformat()
    report_path = os.path.join(a.out_dir, f"classifier-gate-{stamp}.report.json")
    cmd = [sys.executable, os.path.join(HERE, "binoculars_score.py"), "--pairs", trials_path, "--base", a.base,
           "--adapter", adapter, "--out", report_path, "--quiet"]
    rc = subprocess.run(cmd).returncode
    if rc != 0 or not os.path.exists(report_path):
        sys.exit(f"FATAL: binoculars_score.py rc={rc}; no gate file written")
    report = json.load(open(report_path))
    key, auc = extract_auc(report)
    if auc is None:
        sys.exit("FATAL: report has no auc_oriented; no gate file written")
    out = {"date": stamp, "cycle_dir": a.cycle_dir, "n": len(trials), "base": a.base, "adapter": adapter,
           "score_key": key, "auc": auc, "report": report_path,
           "measures": "classifier separability of real-Seth vs adapter text; disagreement with the human gate is signal"}
    path = os.path.join(a.out_dir, f"classifier-gate-{stamp}.json")
    json.dump(out, open(path, "w"), indent=2)
    print(f"[classifier-gate] AUC={auc:.3f} ({key}) n={len(trials)} -> {path}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
