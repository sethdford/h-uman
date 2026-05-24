#!/usr/bin/env python3
"""
CI quality gate — fail a PR / build if persona drift drops below threshold.

Wired into CI (pre-merge or nightly), this replaces an LLM-as-judge for
shipping decisions. The trained speaker-ID classifier delivers a deterministic
P(Seth) per response with zero per-inference cost — perfect for CI, where
LLM-as-judge would be slow, expensive, and non-reproducible.

How to use in CI:

  1. Run any eval suite that produces responses into a JSON file with the shape:
       [{"prompt": "...", "actual_output": "..."}, ...]
     (this matches scripts/eval_*.py output shape)

  2. Invoke:
       python3 scripts/persona_quality_gate.py \
           --responses /tmp/eval_responses.json \
           --classifier /tmp/seth_speaker_id.json \
           --threshold 0.55

  3. Exit codes:
       0 — gate passed (mean P(Seth) >= threshold, no abandons)
       1 — gate failed (drop below threshold or too many empty responses)
       2 — gate could not run (missing inputs)

  4. JSON report at --out for downstream tooling.

Why 0.55 by default: the trained classifier returns 0.5 at the decision
boundary; real Seth responses score 0.7-0.9; AI-assistant shapes score
0.05-0.20. A bar of 0.55 catches anything that doesn't visibly differ
from the trained Seth-voice distribution. A team can ratchet this up
as quality improves; or down for early-stage exploration.

Usage:
  python3 scripts/persona_quality_gate.py --responses path.json
  python3 scripts/persona_quality_gate.py --eval-suite eval_suites/X.json --responses path.json
"""

import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))


def load_clf(path: str):
    """Load the trained speaker-ID classifier from a JSON checkpoint."""
    from personaeval_speaker_id import load_classifier
    return load_classifier(path)


def score_response(clf: dict, text: str) -> float:
    """Return the P(Seth) probability for a single response."""
    from personaeval_speaker_id import p_seth
    return p_seth(clf, text) if text else 0.0


def run_gate(responses: list, clf: dict, threshold: float,
             min_non_empty_rate: float = 0.95) -> dict:
    """Score each response and decide pass/fail.

    Returns:
      {gate_passed: bool, mean_p_seth: float, median_p_seth: float,
       below_threshold_count, non_empty_rate, threshold, n, per_response: [...]}
    """
    per = []
    for r in responses:
        text = r.get("actual_output") or r.get("text") or r.get("response") or ""
        prompt = r.get("prompt") or r.get("input") or ""
        p = score_response(clf, text)
        per.append({"prompt": prompt[:80], "text": text[:120],
                    "p_seth": p, "passes": p >= threshold and bool(text)})

    n = len(per)
    n_non_empty = sum(1 for r in per if r["text"])
    n_pass = sum(1 for r in per if r["passes"])
    p_seth_scores = [r["p_seth"] for r in per if r["text"]]

    non_empty_rate = n_non_empty / n if n else 0
    mean_p_seth = statistics.mean(p_seth_scores) if p_seth_scores else 0
    median_p_seth = statistics.median(p_seth_scores) if p_seth_scores else 0

    # Gate logic
    gate_passed = (
        non_empty_rate >= min_non_empty_rate and
        mean_p_seth >= threshold
    )

    return {
        "gate_passed": gate_passed,
        "n": n,
        "n_non_empty": n_non_empty,
        "non_empty_rate": non_empty_rate,
        "n_pass": n_pass,
        "n_fail": n - n_pass,
        "mean_p_seth": mean_p_seth,
        "median_p_seth": median_p_seth,
        "threshold": threshold,
        "min_non_empty_rate": min_non_empty_rate,
        "below_threshold_examples": [
            r for r in per if not r["passes"]][:10],
        "per_response": per,
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--responses", required=True,
                   help="JSON file with list of {prompt, actual_output}")
    p.add_argument("--classifier", default="/tmp/seth_speaker_id.json")
    p.add_argument("--threshold", type=float, default=0.55,
                   help="Mean P(Seth) below this fails the gate (default 0.55)")
    p.add_argument("--min-non-empty-rate", type=float, default=0.95,
                   help="Fraction of non-empty responses required")
    p.add_argument("--out", default="/tmp/persona_quality_gate.json")
    args = p.parse_args()

    if not Path(args.classifier).exists():
        print(f"ERROR: classifier file not found: {args.classifier}",
              file=sys.stderr)
        print("Hint: train it with `python3 scripts/personaeval_speaker_id.py "
              "--train` first", file=sys.stderr)
        sys.exit(2)

    if not Path(args.responses).exists():
        print(f"ERROR: responses file not found: {args.responses}",
              file=sys.stderr)
        sys.exit(2)

    clf = load_clf(args.classifier)
    raw = json.loads(Path(args.responses).read_text())
    # Accept either a list or a dict-wrapped list
    if isinstance(raw, dict):
        # Try common shapes: {"results": [...]} or {"tasks": [...]}
        responses = raw.get("results") or raw.get("tasks") or list(raw.values())[0]
    else:
        responses = raw

    report = run_gate(responses, clf, args.threshold, args.min_non_empty_rate)

    print()
    print(f"{'GATE PASSED' if report['gate_passed'] else 'GATE FAILED'}")
    print(f"  n responses:        {report['n']}")
    print(f"  non-empty rate:     {report['non_empty_rate']:.1%}  "
          f"(min: {args.min_non_empty_rate:.1%})")
    print(f"  mean P(Seth):       {report['mean_p_seth']:.3f}  "
          f"(threshold: {args.threshold:.2f})")
    print(f"  median P(Seth):     {report['median_p_seth']:.3f}")
    print(f"  passing/{report['n']}: {report['n_pass']}")

    if report["below_threshold_examples"]:
        print()
        print("Below-threshold examples (first 10):")
        for r in report["below_threshold_examples"]:
            print(f"  P={r['p_seth']:.3f} | {r['text']!r}")

    Path(args.out).write_text(json.dumps(report, indent=2))
    print()
    print(f"Full report: {args.out}")
    sys.exit(0 if report["gate_passed"] else 1)


if __name__ == "__main__":
    main()
