#!/usr/bin/env python3
"""Sustained multi-turn coherence eval (on-device).

Drives the LOCAL mlx-server through deep (20–30 turn) conversations and
scores retention (judge + anchors), voice drift (judge, over distance), and
latency (wall-clock total turn latency: ceiling + growth). Emits a verdict
JSON. Nightly/manual tool — not a per-PR CI gate.

Usage:
  python3 scripts/eval_multiturn_local.py \\
    --server-url http://127.0.0.1:8741 \\
    --output-json ~/.human/logs/eval-multiturn-local.json

Exit codes:
  0 = run PASS
  1 = run FAIL (an axis failed, or a scenario fell below the hard retention floor)
  2 = DEFERRED (mlx-server unreachable)
  3 = SKIPPED (judge/ADC unavailable; latency axis ran, qualitative axes skipped)
"""
import argparse
import json
import statistics
import sys
import time
import urllib.request
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))


def _thirds(series):
    """Split a list into (first_third, last_third) by index."""
    n = len(series)
    k = max(1, n // 3)
    return series[:k], series[-k:]


def latency_ceiling_violations(series_ms, ceiling_ms):
    """Return the list of turn indices whose latency exceeds the ceiling."""
    return [i for i, v in enumerate(series_ms) if v > ceiling_ms]


def latency_growth(series_ms):
    """Fractional growth of last-third mean latency vs first-third mean.

    Returns 0.0 for an empty or single-element series. A return of 0.2 means
    the late turns are 20% slower than the early turns.
    """
    if len(series_ms) < 2:
        return 0.0
    first, last = _thirds(series_ms)
    fmean = statistics.mean(first)
    if fmean == 0:
        return 0.0
    return (statistics.mean(last) - fmean) / fmean


def latency_ok(series_ms, ceiling_ms, max_growth):
    """Gate latency on absolute ceiling AND bounded growth.

    Returns (ok: bool, detail: dict).
    """
    violations = latency_ceiling_violations(series_ms, ceiling_ms)
    growth = latency_growth(series_ms)
    ok = (not violations) and (growth <= max_growth)
    detail = {
        "ceiling_ms": ceiling_ms,
        "ceiling_violations": violations,
        "growth": growth,
        "max_growth": max_growth,
        "series_ms": series_ms,
    }
    return ok, detail


def retention_rate(anchor_results):
    """Fraction of anchors the judge marked retained. Empty → 0.0."""
    if not anchor_results:
        return 0.0
    return sum(1 for r in anchor_results if r) / len(anchor_results)
