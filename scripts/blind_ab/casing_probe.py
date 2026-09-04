#!/usr/bin/env python3
"""casing_probe.py — does an adapter's casing/punctuation habit match Seth's?

WHY THIS EXISTS (2026-09-04): production replies measured 86% lowercase-start
/ 0% terminal-punctuation / median 27 chars against real Seth's 6% / 25% /
21 chars on the same contexts. LUAR (the authorship-gap metric this repo
already gates promotion on) did not move on this axis — a persona twin can
sound like Seth in vocabulary and register while still being trivially
distinguishable by "does every reply start with a lowercase letter." This
script is a narrow, fast, deterministic check for exactly that axis, meant
to run ALONGSIDE the LUAR gate, not replace it.

Reads the same {trials: [{context, real_seth, ai_response}, ...]} shape
authorship_gap.py and score_candidate_offline.py already use (the
"classifier_trials" contract — see authorship_gap.py's docstring and
scripts/blind_ab/gen_classifier_trials.py). Casing/punctuation definitions
are the SAME functions the persona's style card uses
(scripts/eval_persona_evolution.py's starts_lowercase / terminal_punctuation)
— a second hand-rolled definition of "starts lowercase" is exactly the kind
of drift .claude/rules/ warns about (style_card_single_source memory note).

Gate (exits non-zero with a clear line on failure):
    adapter lowercase-start rate > --adapter-lowercase-max (default 0.10), OR
    |adapter - human| gap on lowercase-start OR terminal-punct
        exceeds --max-gap (default 0.15)

This script only MEASURES and reports PASS/FAIL — it never promotes,
disables, or otherwise flips anything. See score_candidate_offline.py's
`casing_gate` report field, which calls compute_casing_gate() directly.

Usage:
    casing_probe.py --trials ~/blind_ab_run/classifier_trials.json
    casing_probe.py --trials <candidate_trials.json> --out <report.json>
"""
import argparse
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # blind_ab/ -- for imessage_text
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # scripts/
from eval_persona_evolution import (  # noqa: E402
    starts_lowercase as _starts_lowercase_or_none,
)
from eval_persona_evolution import terminal_punctuation  # noqa: E402

DEFAULT_TRIALS = os.path.expanduser("~/blind_ab_run/classifier_trials.json")
DEFAULT_MIN_TRIALS = 20
DEFAULT_ADAPTER_LOWERCASE_MAX = 0.10
DEFAULT_MAX_GAP = 0.15


def norm(s):
    return (s or "").strip()


def lowercase_rate(texts):
    vals = [v for v in (_starts_lowercase_or_none(t) for t in texts) if v is not None]
    if not vals:
        return 0.0, 0
    return sum(1 for v in vals if v) / len(vals), len(vals)


def punct_rate(texts):
    if not texts:
        return 0.0, 0
    vals = [terminal_punctuation(t) != "none" for t in texts]
    return sum(1 for v in vals if v) / len(vals), len(vals)


def median_length(texts):
    if not texts:
        return 0
    return statistics.median(len(t) for t in texts)


def side_stats(texts):
    lc, lc_n = lowercase_rate(texts)
    pt, pt_n = punct_rate(texts)
    return {
        "n": len(texts),
        "lowercase_start_rate": round(lc, 4),
        "lowercase_start_n": lc_n,
        "terminal_punct_rate": round(pt, 4),
        "median_length": median_length(texts),
    }


def compute_casing_gate(trials, adapter_field="ai_response", human_field="real_seth",
                        adapter_lowercase_max=DEFAULT_ADAPTER_LOWERCASE_MAX,
                        max_gap=DEFAULT_MAX_GAP):
    """trials: list of dicts, each with `adapter_field` and `human_field`
    string keys (the classifier_trials shape). Returns a report dict with a
    "pass" bool and "reasons" list of failing checks — never raises on a
    well-formed trials list, so callers can embed it in a larger report
    without a probe bug taking down the rest of that report."""
    adapter_texts = [norm(t.get(adapter_field)) for t in trials if norm(t.get(adapter_field))]
    human_texts = [norm(t.get(human_field)) for t in trials if norm(t.get(human_field))]

    adapter = side_stats(adapter_texts)
    human = side_stats(human_texts)

    lc_gap = round(abs(adapter["lowercase_start_rate"] - human["lowercase_start_rate"]), 4)
    pt_gap = round(abs(adapter["terminal_punct_rate"] - human["terminal_punct_rate"]), 4)

    reasons = []
    if adapter["lowercase_start_rate"] > adapter_lowercase_max:
        reasons.append(
            f"adapter lowercase-start rate {adapter['lowercase_start_rate']:.4f} "
            f"> --adapter-lowercase-max {adapter_lowercase_max}")
    if lc_gap > max_gap:
        reasons.append(
            f"|adapter-human| lowercase-start gap {lc_gap:.4f} > --max-gap {max_gap}")
    if pt_gap > max_gap:
        reasons.append(
            f"|adapter-human| terminal-punct gap {pt_gap:.4f} > --max-gap {max_gap}")

    return {
        "n_trials": len(trials),
        "adapter": adapter,
        "human": human,
        "gaps": {"lowercase_start_rate": lc_gap, "terminal_punct_rate": pt_gap},
        "thresholds": {"adapter_lowercase_max": adapter_lowercase_max, "max_gap": max_gap},
        "pass": not reasons,
        "reasons": reasons,
    }


def compute_casing_gate_from_file(path, **kwargs):
    """Load a {trials:[...]} (or bare list) JSON file and run
    compute_casing_gate on it. Raises SystemExit (REFUSING) if the file is
    missing/unreadable/too small — the same refusal contract as
    authorship_gap.py's own --trials handling."""
    min_trials = kwargs.pop("min_trials", DEFAULT_MIN_TRIALS)
    if not os.path.isfile(path):
        raise SystemExit(f"REFUSING: trials file not found: {path}; nothing written")
    try:
        data = json.load(open(path))
    except (json.JSONDecodeError, OSError) as e:
        raise SystemExit(f"REFUSING: could not parse {path} ({type(e).__name__}: {e}); nothing written")
    trials = data.get("trials") if isinstance(data, dict) else data
    if not trials:
        raise SystemExit(f"REFUSING: {path} has zero trials; nothing written")
    if len(trials) < min_trials:
        raise SystemExit(f"REFUSING: {len(trials)} trials < --min-trials {min_trials}; nothing written")
    return compute_casing_gate(trials, **kwargs)


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--trials", default=DEFAULT_TRIALS)
    ap.add_argument("--out", default=None)
    ap.add_argument("--min-trials", type=int, default=DEFAULT_MIN_TRIALS)
    ap.add_argument("--adapter-field", default="ai_response")
    ap.add_argument("--human-field", default="real_seth")
    ap.add_argument("--adapter-lowercase-max", type=float, default=DEFAULT_ADAPTER_LOWERCASE_MAX)
    ap.add_argument("--max-gap", type=float, default=DEFAULT_MAX_GAP)
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    report = compute_casing_gate_from_file(
        args.trials, min_trials=args.min_trials,
        adapter_field=args.adapter_field, human_field=args.human_field,
        adapter_lowercase_max=args.adapter_lowercase_max, max_gap=args.max_gap)

    print(json.dumps(report, indent=2))
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
        with open(args.out, "w") as fh:
            json.dump(report, fh, indent=2)
        print(f"wrote {args.out}")

    if not report["pass"]:
        for reason in report["reasons"]:
            print(f"FAIL: {reason}", file=sys.stderr)
        return 1
    print("PASS: casing_gate", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
