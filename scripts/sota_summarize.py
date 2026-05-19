#!/usr/bin/env python3
"""
Summarize the 6-month SOTA runs into ONE table.

Reads JSON outputs from each layer's tool and prints a single
comparison view so we know — at a glance — which layers moved
which metrics on this run.

Inputs (all optional; absent ones reported as N/A):
  /tmp/L5_ttt_results.json         verifier_ttt.py output
  /tmp/L2_ablation_results.json    memory_ablation.py output
  /tmp/M6_ablation_results.json    ablation_orchestrator.py output

Also re-scores chosen texts with the trained PersonaEval classifier
(/tmp/seth_speaker_id.json) so we report P(Seth) alongside shape-score.

Usage:
  python3 scripts/sota_summarize.py
"""

import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))


def load_or_none(p):
    try:
        return json.loads(Path(p).read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def safe_mean(vals):
    vals = [v for v in vals if v is not None]
    return statistics.mean(vals) if vals else None


def rescore_texts(texts):
    """Score a list of texts with the trained speaker-ID classifier."""
    try:
        from personaeval_speaker_id import load_classifier, p_seth
    except ImportError:
        return None
    try:
        clf = load_classifier("/tmp/seth_speaker_id.json")
    except FileNotFoundError:
        return None
    return [p_seth(clf, t) for t in texts if t]


def report():
    l5 = load_or_none("/tmp/L5_ttt_results.json")
    l2 = load_or_none("/tmp/L2_ablation_results.json")
    m6 = load_or_none("/tmp/M6_ablation_results.json")

    rows = []  # (label, mean_shape, mean_p_seth, mean_latency_s, n)

    if l5:
        chosen_scores = [r["chosen_score"] for r in l5]
        chosen_texts = [r["chosen_text"] for r in l5]
        elapsed = [r["total_elapsed_s"] for r in l5]
        first_scores = [r["candidates"][0]["shape"]["score"] for r in l5]
        first_texts = [r["candidates"][0]["text"] for r in l5]
        per_call = (statistics.mean(elapsed) / len(l5[0]["candidates"])
                    if l5 and l5[0]["candidates"] else None)
        rows.append((
            "L5 first-candidate (no TTT)",
            statistics.mean(first_scores) if first_scores else None,
            safe_mean(rescore_texts(first_texts)),
            per_call,
            len(l5),
        ))
        rows.append((
            "L5 best-of-N (TTT)",
            statistics.mean(chosen_scores) if chosen_scores else None,
            safe_mean(rescore_texts(chosen_texts)),
            statistics.mean(elapsed) if elapsed else None,
            len(l5),
        ))

    if l2:
        a_scores = [r["A_full"]["shape"]["score"] for r in l2]
        b_scores = [r["B_persona"]["shape"]["score"] for r in l2]
        a_texts = [r["A_full"]["text"] for r in l2]
        b_texts = [r["B_persona"]["text"] for r in l2]
        a_lat = [r["A_full"]["elapsed_s"] for r in l2]
        b_lat = [r["B_persona"]["elapsed_s"] for r in l2]
        rows.append((
            "L2 A: gateway full memory",
            statistics.mean(a_scores), safe_mean(rescore_texts(a_texts)),
            statistics.mean(a_lat), len(l2),
        ))
        rows.append((
            "L2 B: persona-only (no memory)",
            statistics.mean(b_scores), safe_mean(rescore_texts(b_texts)),
            statistics.mean(b_lat), len(l2),
        ))

    if m6:
        for run in m6.get("runs", []):
            run_label = run["spec"].get("label", run.get("id", "?"))
            s = run["summary"]
            texts = [r["text"] for r in run["results"]]
            rows.append((
                f"M6 {run.get('id', '?')}: {run_label[:40]}",
                s["mean_shape"], safe_mean(rescore_texts(texts)),
                s["mean_latency_s"], s["n"],
            ))

    print()
    print("=" * 96)
    print("SOTA SUMMARY — Layers measured on iMessage humanness suite")
    print("=" * 96)
    print(f"  {'config':<50} {'shape':>7} {'P(Seth)':>9} {'latency':>10} {'n':>4}")
    print("  " + "-" * 86)
    for label, shape, p_seth_val, lat, n in rows:
        sshape = f"{shape:.3f}" if shape is not None else "N/A"
        sp = f"{p_seth_val:.3f}" if p_seth_val is not None else "N/A"
        sl = f"{lat:.1f}s" if lat is not None else "N/A"
        print(f"  {label[:50]:<50} {sshape:>7} {sp:>9} {sl:>10} {n:>4}")
    print()

    if l5:
        first = next((r for r in rows if r[0].endswith("(no TTT)")), None)
        best = next((r for r in rows if r[0].endswith("(TTT)")), None)
        if first and best and first[1] is not None and best[1] is not None:
            print(f"L5 lift (best-of-N vs first):   "
                  f"Δshape = {best[1] - first[1]:+.3f}   "
                  f"Δlatency = {best[3] - first[3]:+.1f}s/turn (×{best[4]} compute)")
            if best[2] is not None and first[2] is not None:
                print(f"L5 lift in P(Seth):              "
                      f"Δ = {best[2] - first[2]:+.3f}")
    if l2:
        a = next((r for r in rows if r[0].startswith("L2 A")), None)
        b = next((r for r in rows if r[0].startswith("L2 B")), None)
        if a and b:
            ds = (a[1] or 0) - (b[1] or 0)
            dl = (a[3] or 0) - (b[3] or 0)
            print(f"L2 cost (full memory vs persona-only):  "
                  f"Δshape = {ds:+.3f}   Δlatency = {dl:+.1f}s")
            print(f"  -> if shape delta ~ 0 and latency delta > 0, "
                  f"memory is dead weight; refactor to RAG.")
    print()


if __name__ == "__main__":
    report()
