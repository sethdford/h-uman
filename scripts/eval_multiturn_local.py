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
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

# --- Thresholds (calibration seeds — Task 9 locks the real numbers) ---
RETENTION_RATE_MIN     = 0.85
RETENTION_HARD_FLOOR   = 0.70
VOICE_DRIFT_TOL        = 0.10
LATENCY_CEILING_MS     = 8000.0
LATENCY_MAX_GROWTH     = 0.20
RUN_PASS_MIN_SCENARIOS = 5


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


def voice_normalize(overall_score_1_to_10):
    """Normalize a judge overall_score (1–10) to [0,1]."""
    return float(overall_score_1_to_10) / 10.0


def voice_drift_ok(first_third_norm, last_third_norm, tol, any_hard_ai):
    """Voice axis passes when late-conversation voice has not decayed.

    Fails if the last-third normalized score dropped more than `tol` below the
    first-third, OR any late turn flipped to a hard AI verdict.
    """
    if any_hard_ai:
        return False
    return last_third_norm >= (first_third_norm - tol)


def scenario_verdict(name, retention, voice_pass, voice_detail, latency_pass, latency_detail):
    """Assemble a single scenario's per-axis verdict. All three axes must pass."""
    retention_pass = retention >= RETENTION_RATE_MIN
    passed = retention_pass and voice_pass and latency_pass
    return {
        "scenario": name,
        "retention": {"rate": retention, "min": RETENTION_RATE_MIN, "passed": retention_pass},
        "voice": {"passed": voice_pass, **voice_detail},
        "latency": {"passed": latency_pass, **latency_detail},
        "passed": passed,
    }


def run_verdict(scenario_verdicts):
    """Aggregate scenario verdicts into the run-level verdict.

    Run passes when ≥ RUN_PASS_MIN_SCENARIOS scenarios pass AND no scenario
    fell below RETENTION_HARD_FLOOR (a catastrophic-retention veto).
    """
    passed_count = sum(1 for sv in scenario_verdicts if sv["passed"])
    hard_floor_veto = any(
        sv["retention"]["rate"] < RETENTION_HARD_FLOOR for sv in scenario_verdicts)
    run_passed = (passed_count >= RUN_PASS_MIN_SCENARIOS) and not hard_floor_veto
    return {
        "scenarios": scenario_verdicts,
        "scenarios_passed": passed_count,
        "scenarios_total": len(scenario_verdicts),
        "min_to_pass": RUN_PASS_MIN_SCENARIOS,
        "hard_floor_veto": hard_floor_veto,
        "run_passed": run_passed,
    }


def write_verdict(verdict, path):
    """Write the verdict JSON, stamping generated_at. Creates parent dirs."""
    out = dict(verdict)
    out.setdefault("generated_at", datetime.utcnow().isoformat() + "Z")
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(out, indent=2))


class BackendUnreachable(RuntimeError):
    """Raised when the local mlx-server cannot be reached. Never fall back to cloud."""


class LocalBackend:
    """Talks to the local mlx-server's OpenAI-compatible endpoint.

    Sends the FULL accumulated history each turn (mirrors compatible.c — no
    server-side caching), which is what makes the latency growth signal real.
    """
    def __init__(self, url, model="default", temperature=0.9, timeout=120):
        self.url = url.rstrip("/")
        self.model = model
        self.temperature = temperature
        self.timeout = timeout

    def chat(self, messages):
        """POST messages, return (content, latency_ms). Raises BackendUnreachable."""
        body = json.dumps({
            "model": self.model,
            "messages": messages,
            "temperature": self.temperature,
        }).encode()
        req = urllib.request.Request(
            f"{self.url}/v1/chat/completions", data=body,
            headers={"Content-Type": "application/json"})
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                data = json.loads(resp.read())
        except (OSError, urllib.error.URLError) as e:
            raise BackendUnreachable(f"{self.url}: {e}") from e
        latency_ms = (time.time() - t0) * 1000.0
        content = data["choices"][0]["message"]["content"]
        return content, latency_ms
