#!/usr/bin/env python3
"""Score completed blind-2AFC rating sheets against the private answer key.

Detection = rater picked the SETH (real) option for an item.
  0.50 -> indistinguishable (GOAL).  1.00 -> always caught.

Reports, per the protocol:
  - raw detection rate + 95% Wilson CI
  - confidence-weighted detection (vote weight = confidence/3, 1..5 -> ~0.33..1.67)
  - per-rater detection
  - PASS/FAIL vs the two criteria (detection <= 0.60 AND Wilson lower <= 0.55)

Usage:
    python3 score.py sheet_alice.csv sheet_bob.csv --key answer_key.json
    python3 score.py --selftest        # verify the math on synthetic data
"""
import argparse, csv, json, math, sys


def wilson(k, n, z=1.96):
    """95% Wilson score interval for a binomial proportion."""
    if n == 0:
        return (0.0, 0.0, 0.0)
    p = k / n
    d = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / d
    half = (z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))) / d
    return (p, max(0.0, centre - half), min(1.0, centre + half))


def likert_to_01(likert_val):
    """Convert Likert [1-5] rating to [0-1] scale."""
    try:
        val = float(likert_val)
    except (ValueError, TypeError):
        return None
    if val < 1.0 or val > 5.0:
        return None
    return (val - 1.0) / 4.0


def score_axes(rows):
    """Compute per-axis scores from Likert responses.

    Returns dict: {axis_name: mean_score_in_[0,1], ...}
    Also returns per_rater_per_axis for observability.
    """
    axis_names = ["opinion", "memory", "reasoning", "lexical", "tone", "syntax"]
    axis_sums = {ax: 0.0 for ax in axis_names}
    axis_counts = {ax: 0 for ax in axis_names}
    per_rater_axes = {}

    for r in rows:
        rater = r.get("_rater", "?")
        if rater not in per_rater_axes:
            per_rater_axes[rater] = {ax: [] for ax in axis_names}

        for ax in axis_names:
            col = f"axis_{ax}"
            val = likert_to_01(r.get(col) or "")
            if val is not None:
                axis_sums[ax] += val
                axis_counts[ax] += 1
                per_rater_axes[rater][ax].append(val)

    # Aggregate per-axis means
    result = {}
    for ax in axis_names:
        if axis_counts[ax] > 0:
            result[ax] = axis_sums[ax] / axis_counts[ax]
        else:
            result[ax] = 0.0

    return result


def score_rows(rows, key):
    """rows: list of {id, choice, confidence, _rater?}. Returns aggregate dict."""
    n = wsum = wdet = det = 0
    per_rater = {}
    for r in rows:
        rid = r["id"]; choice = (r.get("choice") or "").strip().upper()
        if rid not in key or choice not in ("A", "B"):
            continue  # unanswered / unknown item -> skip
        try:
            conf = float(r.get("confidence") or 3)
        except ValueError:
            conf = 3.0
        conf = min(5.0, max(1.0, conf))
        w = conf / 3.0
        correct = 1 if choice == key[rid] else 0     # picked the real Seth?
        n += 1; det += correct
        wsum += w; wdet += w * correct
        pr = per_rater.setdefault(r.get("_rater", "?"), [0, 0])
        pr[0] += correct; pr[1] += 1
    p, lo, hi = wilson(det, n)
    wrate = (wdet / wsum) if wsum else 0.0
    return {"n": n, "detect": p, "ci_lo": lo, "ci_hi": hi,
            "weighted_detect": wrate,
            "per_rater": {k: (c / t if t else 0.0, t) for k, (c, t) in per_rater.items()}}


def load_sheets(paths):
    rows = []
    for path in paths:
        rater = path.split("/")[-1].replace("rating_sheet_", "").replace(".csv", "")
        with open(path) as f:
            for r in csv.DictReader(f):
                r["_rater"] = rater
                rows.append(r)
    return rows


def report(agg):
    print(f"items scored      : {agg['n']}")
    print(f"detection rate    : {agg['detect']:.3f}   (0.50 = indistinguishable)")
    print(f"  95% Wilson CI   : [{agg['ci_lo']:.3f}, {agg['ci_hi']:.3f}]")
    print(f"confidence-wtd    : {agg['weighted_detect']:.3f}")
    print("per-rater detection:")
    for k, (rate, t) in sorted(agg["per_rater"].items()):
        print(f"  {k:12} {rate:.3f}  (n={t})")
    crit1 = agg["detect"] <= 0.60
    crit2 = agg["ci_lo"] <= 0.55
    crit3 = agg["weighted_detect"] <= 0.60
    verdict = "PASS" if (crit1 and crit2 and crit3) else "FAIL"
    print(f"\nRESULT_blind_ab={verdict}  "
          f"(detect<=0.60:{crit1}  wilson_lo<=0.55:{crit2}  wtd<=0.60:{crit3})")
    return verdict


def selftest():
    import random
    key = {f"t{i}": ("A" if i % 2 else "B") for i in range(200)}
    rng = random.Random(1)
    # Simulate an indistinguishable bot: raters guess at chance.
    rows = []
    for i in range(200):
        choice = rng.choice(("A", "B"))
        rows.append({"id": f"t{i}", "choice": choice, "confidence": rng.randint(1, 5),
                     "axis_opinion": 3, "axis_memory": 3, "axis_reasoning": 3,
                     "axis_lexical": 3, "axis_tone": 3, "axis_syntax": 3,
                     "_rater": f"r{i % 5}"})
    agg = score_rows(rows, key)
    assert 0.40 <= agg["detect"] <= 0.60, agg["detect"]
    assert agg["n"] == 200
    # Perfect-detector case -> detection 1.0, PASS must be False.
    perfect = [{"id": k, "choice": v, "confidence": 5,
                "axis_opinion": 5, "axis_memory": 5, "axis_reasoning": 5,
                "axis_lexical": 5, "axis_tone": 5, "axis_syntax": 5,
                "_rater": "x"} for k, v in key.items()]
    pagg = score_rows(perfect, key)
    assert abs(pagg["detect"] - 1.0) < 1e-9, pagg["detect"]
    assert pagg["ci_lo"] > 0.55
    # Wilson sanity: 0/0 safe, 1/1 within [0,1].
    assert wilson(0, 0) == (0.0, 0.0, 0.0)
    lo_ok = 0.0 <= wilson(1, 1)[1] <= 1.0
    assert lo_ok

    # Test Likert → [0,1] conversion
    assert likert_to_01(1) == 0.0, "Likert 1 should map to 0.0"
    assert likert_to_01(3) == 0.5, "Likert 3 should map to 0.5"
    assert likert_to_01(5) == 1.0, "Likert 5 should map to 1.0"
    assert likert_to_01("") is None, "Empty string should be None"
    assert likert_to_01("invalid") is None, "Invalid string should be None"

    # Test per-axis aggregation with all-neutral ratings
    neutral_rows = [{"id": f"t{i}", "axis_opinion": 3, "axis_memory": 3,
                     "axis_reasoning": 3, "axis_lexical": 3, "axis_tone": 3,
                     "axis_syntax": 3, "_rater": f"r{i % 3}"} for i in range(15)]
    axes = score_axes(neutral_rows)
    for ax in ["opinion", "memory", "reasoning", "lexical", "tone", "syntax"]:
        assert abs(axes[ax] - 0.5) < 1e-9, f"Axis {ax} with all 3's should be 0.5, got {axes[ax]}"

    # Test per-axis with mixed ratings
    mixed_rows = [
        {"axis_opinion": 5, "axis_memory": 1, "axis_reasoning": 3,
         "axis_lexical": 5, "axis_tone": 5, "axis_syntax": 3, "_rater": "r1"},
        {"axis_opinion": 5, "axis_memory": 1, "axis_reasoning": 3,
         "axis_lexical": 5, "axis_tone": 5, "axis_syntax": 3, "_rater": "r2"},
    ]
    axes = score_axes(mixed_rows)
    assert abs(axes["opinion"] - 1.0) < 1e-9, f"opinion should be 1.0, got {axes['opinion']}"
    assert abs(axes["memory"] - 0.0) < 1e-9, f"memory should be 0.0, got {axes['memory']}"
    assert abs(axes["reasoning"] - 0.5) < 1e-9, f"reasoning should be 0.5, got {axes['reasoning']}"

    # Test backward compatibility: legacy keys still present
    test_rows = [{"id": "t1", "choice": "A", "confidence": 5,
                  "axis_opinion": 5, "axis_memory": 5, "axis_reasoning": 5,
                  "axis_lexical": 5, "axis_tone": 5, "axis_syntax": 5,
                  "_rater": "test"}]
    test_key = {"t1": "A"}
    agg = score_rows(test_rows, test_key)
    assert "detect" in agg, "Legacy 'detect' field missing"
    assert "n" in agg, "Legacy 'n' field missing"
    assert "ci_lo" in agg, "Legacy 'ci_lo' field missing"
    assert "ci_hi" in agg, "Legacy 'ci_hi' field missing"

    print("selftest OK: Likert conversion, axis aggregation, backward-compat verified")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheets", nargs="*")
    ap.add_argument("--key")
    ap.add_argument("--json-out", help="Write JSON output to this file")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest(); return
    if not a.sheets or not a.key:
        print("need sheets + --key (or --selftest)", file=sys.stderr); sys.exit(2)
    with open(a.key) as f:
        key = json.load(f)
    rows = load_sheets(a.sheets)
    agg = score_rows(rows, key)
    axes = score_axes(rows)

    # Backward-compatible output: keep legacy keys, add axes object
    agg["axes"] = axes

    verdict = report(agg)
    if a.json_out:
        with open(a.json_out, 'w') as f:
            json.dump(agg, f, indent=2)
    sys.exit(0 if verdict == "PASS" else 1)


if __name__ == "__main__":
    main()
