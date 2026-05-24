#!/usr/bin/env python3
"""
Phase E4 (2026-05-18) — drift detection + auto-rollback policy.

Closes an operational gap from the audit: we promote every adapter
the loop produces, with no detector for whether the new adapter is
DEGRADING production behavior. Without this, a bad LoRA stays live
until a human notices.

What this script does:
  1. Reads the lineage manifest (adapter_lineage.jsonl) — D2's output
  2. Reads the outcomes JSONL — the post-adapter inference traffic
  3. Compares outcome quality signals WINDOWED by adapter swap time
  4. Computes drift metrics across windows:
       - Guard decision distribution (PASS vs REWRITE vs REJECT)
       - Average response length
       - Per-contact reach
       - Latency distribution
  5. Decides: drift_status = OK | DEGRADING | NEEDS_ROLLBACK with reasons
  6. (Optional) Emits a rollback hint pointing at the prior adapter

What it does NOT do (out of scope):
  - Trigger actual rollback (operator decides; this is an advisor)
  - Re-train (that's the loop's job)
  - Run live inference (the metrics are post-hoc from observed outcomes)

This is a "monitor what we already have, surface anomalies" tool —
no new data collection required.

Usage:
    python3 scripts/m3_drift_detector.py                   # text report
    python3 scripts/m3_drift_detector.py --json            # machine-readable
    make m3-drift

Exit codes:
    0 — drift status produced (regardless of OK/DEGRADING/NEEDS_ROLLBACK)
    2 — fatal: lineage manifest missing AND outcomes JSONL missing
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

HUMAN_HOME = Path.home() / ".human"
LINEAGE_PATH = HUMAN_HOME / "training-data" / "adapter_lineage.jsonl"
OUTCOMES_JSONL = HUMAN_HOME / "training-data" / "m3-outcomes.jsonl"


def _read_jsonl(p: Path) -> list[dict]:
    if not p.exists():
        return []
    out = []
    for line in p.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def windows_from_lineage(lineage: list[dict], end_ms: int | None = None) -> list[dict]:
    """Convert lineage entries into adapter-time windows.

    Each window:
        {"adapter_path": "...", "start_ts_ms": int, "end_ts_ms": int}

    The end_ts of window N is start_ts of window N+1, or `end_ms`
    (default: now) for the most recent window. This gives us
    [start, end) intervals to bucket outcomes into."""
    if not lineage:
        return []
    if end_ms is None:
        end_ms = int(time.time() * 1000)
    # Sort by timestamp ascending so we can chain windows.
    sorted_lineage = sorted(lineage, key=lambda e: e.get("timestamp", 0))
    windows = []
    for i, entry in enumerate(sorted_lineage):
        start = entry.get("timestamp", 0)
        end = (sorted_lineage[i + 1].get("timestamp", end_ms)
               if i + 1 < len(sorted_lineage) else end_ms)
        windows.append({
            "adapter_path": entry.get("adapter_path", "?"),
            "kind": entry.get("kind", "?"),
            "start_ts_ms": start,
            "end_ts_ms": end,
        })
    return windows


def bucket_outcomes(outcomes: list[dict], windows: list[dict]) -> dict:
    """For each window, compute summary stats over the outcomes that
    fall into [start_ts_ms, end_ts_ms). Returns:
        {adapter_path: {count, guard_pass_pct, guard_rewrite_pct,
                        avg_completion_tokens, avg_latency_ms,
                        unique_contacts}}
    """
    by_window: dict[str, dict] = {}
    for w in windows:
        bucket = [o for o in outcomes
                  if w["start_ts_ms"] <= o.get("t", 0) < w["end_ts_ms"]]
        if not bucket:
            by_window[w["adapter_path"]] = {
                "count": 0,
                "window_start": w["start_ts_ms"],
                "window_end": w["end_ts_ms"],
                "kind": w["kind"],
            }
            continue
        pass_n = sum(1 for o in bucket if o.get("g", 0) == 1)
        rewrite_n = sum(1 for o in bucket if o.get("g", 0) == 2)
        reject_n = sum(1 for o in bucket if o.get("g", 0) == 3)
        total = len(bucket)
        by_window[w["adapter_path"]] = {
            "count": total,
            "window_start": w["start_ts_ms"],
            "window_end": w["end_ts_ms"],
            "kind": w["kind"],
            "guard_pass_pct": pass_n / total,
            "guard_rewrite_pct": rewrite_n / total,
            "guard_reject_pct": reject_n / total,
            "avg_completion_tokens": sum(o.get("ct", 0) for o in bucket) / total,
            "avg_latency_ms": sum(o.get("l", 0) for o in bucket) / total,
            "unique_contacts": len({o.get("ch", 0) for o in bucket
                                    if o.get("ch", 0) > 0}),
        }
    return by_window


def diagnose_drift(windowed: dict, lineage: list[dict]) -> dict:
    """Compare the most-recent adapter's metrics against the
    second-most-recent. Returns:
        {status: 'OK' | 'DEGRADING' | 'NEEDS_ROLLBACK',
         reasons: [str, ...],
         current_adapter: ...,
         prior_adapter: ...}

    Detection thresholds (conservative; meant to flag, not auto-act):
      - guard_rewrite_pct increased by >10 pts → DEGRADING
        (more outputs need guard intervention = adapter quality dropped)
      - guard_reject_pct increased by >5 pts → NEEDS_ROLLBACK
        (outputs being rejected outright is a hard regression)
      - avg_completion_tokens dropped by >40% → DEGRADING
        (responses shortened — either over-fitting to terse training
        data or the adapter is broken)
      - sample count == 0 in current window → status OK with
        "insufficient data" note (not enough traffic to judge)
    """
    if not lineage or len(lineage) < 2:
        return {
            "status": "OK",
            "reasons": ["need at least 2 adapters in lineage to compare"],
            "current_adapter": None,
            "prior_adapter": None,
        }

    sorted_lineage = sorted(lineage, key=lambda e: e.get("timestamp", 0))
    current = sorted_lineage[-1]
    prior = sorted_lineage[-2]
    cur_metrics = windowed.get(current.get("adapter_path", "?"), {})
    prior_metrics = windowed.get(prior.get("adapter_path", "?"), {})

    if cur_metrics.get("count", 0) == 0:
        return {
            "status": "OK",
            "reasons": [f"no outcomes observed for current adapter "
                        f"{current.get('adapter_path')} (newly-promoted; "
                        f"need traffic to judge)"],
            "current_adapter": current.get("adapter_path"),
            "prior_adapter": prior.get("adapter_path"),
            "current_metrics": cur_metrics,
            "prior_metrics": prior_metrics,
        }

    reasons = []
    status = "OK"

    # Guard-reject regression — hardest signal
    cur_reject = cur_metrics.get("guard_reject_pct", 0)
    prior_reject = prior_metrics.get("guard_reject_pct", 0)
    if cur_reject - prior_reject > 0.05:
        status = "NEEDS_ROLLBACK"
        reasons.append(
            f"guard REJECT rate jumped {prior_reject:.1%} → {cur_reject:.1%} "
            f"(+{(cur_reject - prior_reject) * 100:.1f} pts) — "
            f"outputs are getting rejected outright")

    # Guard-rewrite drift — softer signal
    cur_rewrite = cur_metrics.get("guard_rewrite_pct", 0)
    prior_rewrite = prior_metrics.get("guard_rewrite_pct", 0)
    if cur_rewrite - prior_rewrite > 0.10:
        if status != "NEEDS_ROLLBACK":
            status = "DEGRADING"
        reasons.append(
            f"guard REWRITE rate jumped {prior_rewrite:.1%} → {cur_rewrite:.1%} "
            f"(+{(cur_rewrite - prior_rewrite) * 100:.1f} pts) — "
            f"more outputs need guard repair")

    # Response shortening — softer signal
    cur_tok = cur_metrics.get("avg_completion_tokens", 0)
    prior_tok = prior_metrics.get("avg_completion_tokens", 0)
    if prior_tok > 0 and cur_tok < prior_tok * 0.60:
        if status != "NEEDS_ROLLBACK":
            status = "DEGRADING"
        reasons.append(
            f"avg completion tokens dropped {prior_tok:.0f} → {cur_tok:.0f} "
            f"(-{(1 - cur_tok / prior_tok) * 100:.0f}%) — responses got "
            f"much shorter, possible over-fit")

    if not reasons:
        reasons.append("all monitored signals within tolerance")

    return {
        "status": status,
        "reasons": reasons,
        "current_adapter": current.get("adapter_path"),
        "prior_adapter": prior.get("adapter_path"),
        "current_metrics": cur_metrics,
        "prior_metrics": prior_metrics,
        "rollback_hint": (prior.get("adapter_path")
                          if status == "NEEDS_ROLLBACK" else None),
    }


def format_human(diagnosis: dict, windowed: dict) -> str:
    lines = []
    lines.append("═" * 60)
    lines.append(f"  M3 DRIFT DETECTOR — {diagnosis['status']}")
    lines.append("═" * 60)
    lines.append("\n  Reasons:")
    for r in diagnosis["reasons"]:
        lines.append(f"    - {r}")
    if diagnosis.get("current_adapter"):
        lines.append(f"\n  Current adapter: {diagnosis['current_adapter']}")
        cm = diagnosis.get("current_metrics", {})
        if cm.get("count", 0) > 0:
            lines.append(f"    samples:    {cm.get('count', 0)}")
            lines.append(f"    PASS rate:  {cm.get('guard_pass_pct', 0):.1%}")
            lines.append(f"    REWRITE:    {cm.get('guard_rewrite_pct', 0):.1%}")
            lines.append(f"    REJECT:     {cm.get('guard_reject_pct', 0):.1%}")
            lines.append(f"    avg ct:     {cm.get('avg_completion_tokens', 0):.0f} tokens")
            lines.append(f"    avg lat:    {cm.get('avg_latency_ms', 0):.0f} ms")
    if diagnosis.get("prior_adapter"):
        lines.append(f"\n  Prior adapter:   {diagnosis['prior_adapter']}")
        pm = diagnosis.get("prior_metrics", {})
        if pm.get("count", 0) > 0:
            lines.append(f"    samples:    {pm.get('count', 0)}")
            lines.append(f"    PASS rate:  {pm.get('guard_pass_pct', 0):.1%}")
            lines.append(f"    REWRITE:    {pm.get('guard_rewrite_pct', 0):.1%}")
    if diagnosis.get("rollback_hint"):
        lines.append(f"\n  ROLLBACK HINT: swap to {diagnosis['rollback_hint']}")
        lines.append(f"    (operator decides; this advisor does not act)")
    lines.append("\n" + "═" * 60)
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--lineage", type=Path, default=LINEAGE_PATH)
    ap.add_argument("--outcomes", type=Path, default=OUTCOMES_JSONL)
    ap.add_argument("--json", action="store_true",
                    help="Output JSON (verdict + metrics) instead of text")
    args = ap.parse_args()

    lineage = _read_jsonl(args.lineage)
    outcomes = _read_jsonl(args.outcomes)

    if not lineage and not outcomes:
        print("ERROR: both lineage and outcomes are missing — nothing to compare",
              file=sys.stderr)
        return 2

    windows = windows_from_lineage(lineage)
    windowed = bucket_outcomes(outcomes, windows)
    diagnosis = diagnose_drift(windowed, lineage)

    if args.json:
        print(json.dumps({
            "diagnosis": diagnosis,
            "windowed": windowed,
        }, indent=2, default=str))
    else:
        print(format_human(diagnosis, windowed))

    return 0


if __name__ == "__main__":
    sys.exit(main())
