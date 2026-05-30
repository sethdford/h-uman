#!/usr/bin/env python3
"""humanness_compose.py — compose the 4-axis humanness composite + hybrid gate.

Part of the humanness north-star metric
(docs/plans/2026-05-29-humanness-north-star-metric/, Phase 3 / T6-T8).

Reads the per-axis JSON emitted by `human eval score` (A1 fidelity, A2 anti_ai,
A4 relationship) plus an optional judge JSON (A3), computes a single composite
score, and applies the HYBRID gate the owner chose (2026-05-29):

    FAIL if ANY axis regressed past its tolerance below the trailing baseline,
         OR the composite is below the absolute floor.
    PASS if all axes are within tolerance AND composite >= floor.
    SKIP if there is no baseline yet (bootstrap window not filled).

This catches a one-axis collapse that a single weighted average would mask —
the same shape as the project's classifier-score-plus-flag-gate rule.

Stdlib only (json, statistics, argparse) — matches the nightly harness.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

# Default axis weights (config-overridable, echoed into the verdict for
# provenance). Sum to 1.0.
DEFAULT_WEIGHTS = {
    "fidelity": 0.30,
    "anti_ai": 0.25,
    "judge": 0.20,
    "relationship": 0.25,
}
DEFAULT_TOLERANCE = 0.05      # per-axis no-regression band below baseline
DEFAULT_COMPOSITE_FLOOR = 0.70
DEFAULT_BASELINE_WINDOW = 7   # trailing nights for the median baseline

# Axes that may legitimately be absent from a run (judge needs a reachable LLM;
# relationship needs target-tagged fixtures). When an axis is unavailable its
# weight is redistributed proportionally across the available axes.
OPTIONAL_AXES = ("judge", "relationship")


def axis_available(axes: dict, name: str) -> bool:
    ax = axes.get(name)
    if not ax:
        return False
    if ax.get("available") is False:
        return False
    # An axis with zero contributing rows is not a usable signal.
    return ax.get("n", 0) > 0


def compute_composite(axes: dict, weights: dict | None = None) -> tuple[float, dict]:
    """Weighted composite over available axes; absent axes' weight is
    redistributed proportionally. Returns (composite, used_weights)."""
    weights = dict(weights or DEFAULT_WEIGHTS)
    available = {k: w for k, w in weights.items() if axis_available(axes, k)}
    total_w = sum(available.values())
    if total_w <= 0:
        return 0.0, {}
    used = {k: w / total_w for k, w in available.items()}
    composite = sum(used[k] * float(axes[k]["mean"]) for k in used)
    return composite, used


def load_baseline(path: str | None, window: int) -> dict | None:
    """Trailing per-axis median over the last `window` history rows. Returns
    None when there is no history file or fewer than `window` rows (bootstrap)."""
    if not path:
        return None
    p = Path(path).expanduser()
    if not p.exists():
        return None
    rows = []
    for line in p.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    if len(rows) < window:
        return None  # bootstrap — not enough history to gate against
    recent = rows[-window:]
    baseline: dict = {}
    for axis in DEFAULT_WEIGHTS:
        vals = [r["axes"][axis] for r in recent if axis in r.get("axes", {})]
        if vals:
            baseline[axis] = statistics.median(vals)
    comps = [r["composite"] for r in recent if "composite" in r]
    if comps:
        baseline["composite"] = statistics.median(comps)
    return baseline


def decide_gate(axes: dict, composite: float, baseline: dict | None,
                tolerance: float, composite_floor: float) -> dict:
    """Hybrid gate. Returns a verdict dict with per-axis pass flags + reasons."""
    if baseline is None:
        return {"verdict": "SKIP", "reason": "no baseline yet (bootstrap window not filled)",
                "per_axis": {}}

    per_axis = {}
    reasons = []
    any_regressed = False
    for axis in DEFAULT_WEIGHTS:
        if not axis_available(axes, axis) or axis not in baseline:
            per_axis[axis] = {"checked": False}
            continue
        mean = float(axes[axis]["mean"])
        base = float(baseline[axis])
        passed = mean >= base - tolerance
        per_axis[axis] = {"checked": True, "mean": mean, "baseline": base, "pass": passed}
        if not passed:
            any_regressed = True
            reasons.append(
                f"{axis} regressed: {mean:.3f} < baseline {base:.3f} - tol {tolerance:.3f}")

    floor_ok = composite >= composite_floor
    if not floor_ok:
        reasons.append(f"composite {composite:.3f} < floor {composite_floor:.3f}")

    verdict = "PASS" if (not any_regressed and floor_ok) else "FAIL"
    return {"verdict": verdict, "reason": "; ".join(reasons) or "all axes within tolerance",
            "per_axis": per_axis, "composite_floor_pass": floor_ok}


def append_baseline(path: str | None, axes: dict, composite: float, timestamp: str | None) -> None:
    if not path:
        return
    means = {a: float(axes[a]["mean"]) for a in DEFAULT_WEIGHTS if axis_available(axes, a)}
    row = {"axes": means, "composite": composite}
    if timestamp:
        row["timestamp"] = timestamp
    p = Path(path).expanduser()
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("a") as f:
        f.write(json.dumps(row) + "\n")


def compose(axes: dict, baseline: dict | None, weights: dict | None = None,
            tolerance: float = DEFAULT_TOLERANCE,
            composite_floor: float = DEFAULT_COMPOSITE_FLOOR) -> dict:
    """Full composition: composite + gate verdict, as a single dict."""
    composite, used = compute_composite(axes, weights)
    gate = decide_gate(axes, composite, baseline, tolerance, composite_floor)
    return {
        "composite": composite,
        "weights": used,
        "axes": axes,
        "humanness_verdict": gate["verdict"],
        "gate": gate,
    }


def _merge_judge(axes: dict, judge_path: str | None) -> dict:
    """Fold an optional judge.json (single 'mean'/'n' or 12-dim pass list) into
    the axes dict as the A3 'judge' axis."""
    if not judge_path:
        return axes
    p = Path(judge_path).expanduser()
    if not p.exists():
        axes.setdefault("judge", {"available": False, "n": 0})
        return axes
    data = json.loads(p.read_text())
    if "mean" in data:
        mean, n = float(data["mean"]), int(data.get("n", 1))
    elif "dimensions" in data:  # list of {pass: bool}
        dims = data["dimensions"]
        n = len(dims)
        mean = (sum(1 for d in dims if d.get("pass")) / n) if n else 0.0
    else:
        axes.setdefault("judge", {"available": False, "n": 0})
        return axes
    axes["judge"] = {"mean": mean, "n": n, "available": n > 0}
    return axes


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Compose the humanness composite + gate.")
    ap.add_argument("--axes", required=True, help="axes.json from `human eval score`")
    ap.add_argument("--judge", help="optional judge.json (A3)")
    ap.add_argument("--baseline", help="trailing-baseline JSONL (read + appended)")
    ap.add_argument("--window", type=int, default=DEFAULT_BASELINE_WINDOW)
    ap.add_argument("--tolerance", type=float, default=DEFAULT_TOLERANCE)
    ap.add_argument("--floor", type=float, default=DEFAULT_COMPOSITE_FLOOR)
    ap.add_argument("--timestamp", help="ISO timestamp to stamp the baseline row")
    ap.add_argument("--out", help="write verdict JSON here (default stdout)")
    ap.add_argument("--no-append", action="store_true", help="do not append to baseline")
    args = ap.parse_args(argv)

    axes = json.loads(Path(args.axes).expanduser().read_text()).get("axes", {})
    axes = _merge_judge(axes, args.judge)
    baseline = load_baseline(args.baseline, args.window)
    result = compose(axes, baseline, tolerance=args.tolerance, composite_floor=args.floor)

    if args.baseline and not args.no_append:
        append_baseline(args.baseline, axes, result["composite"], args.timestamp)

    out = json.dumps(result, indent=2)
    if args.out:
        Path(args.out).expanduser().write_text(out + "\n")
    else:
        print(out)

    # Exit code mirrors the nightly gate convention: 0 PASS/SKIP, 1 FAIL.
    return 1 if result["humanness_verdict"] == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
