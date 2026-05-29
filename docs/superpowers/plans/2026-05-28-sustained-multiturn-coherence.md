---
title: Sustained Multi-Turn Coherence On-Device
---

# Sustained Multi-Turn Coherence On-Device — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a nightly/manual eval tool that drives the local mlx-server through 6 deepened (20–30 turn) conversations and measures sustained coherence on three axes — context retention, voice drift, latency — emitting a diagnosable verdict JSON.

**Architecture:** A new isolated harness `scripts/eval_multiturn_local.py` imports the judge + dimensions from the existing `scripts/eval_multiturn.py`, drives a `LocalBackend` that POSTs full accumulated history to the local mlx-server (`http://127.0.0.1:8741/v1/chat/completions`), times each turn, scores retention (per-anchor, judge) and voice drift (judge, first-third vs last-third), gates latency (ceiling + growth), and writes a verdict artifact. Deep scenario data lives in a data-only module `scripts/multiturn_scenarios_deep.py`. All scoring is decomposed into pure functions so the unit tests never touch the live model or judge.

**Tech Stack:** Python 3 (stdlib only: `json`, `urllib`, `time`, `statistics`, `argparse`, `pathlib`, `unittest.mock`); reuses `eval_multiturn.py` (Gemini judge via ADC) and the live mlx-server HTTP endpoint.

---

## Conventions for this plan

- **Test runner pattern (match the repo):** h-uman Python tests are plain modules with `def test_*()` functions and a `main()` at the bottom that runs each, prints `✓`/`✗`, and `sys.exit(0 if not failed else 1)`. Run with `python3 scripts/test_eval_multiturn_local.py`. No pytest, no importlib (filenames use underscores → direct import).
- **Latency metric:** the harness measures **total turn latency** (wall-clock round-trip of the non-streaming POST). True first-token latency needs the unbuffered SSE path (production streaming is buffered), so it is out of scope here; the ceiling+growth math is metric-agnostic. This is the one deliberate refinement of the approved design.
- **All thresholds are calibration seeds** (Task 9 locks the real numbers from the first live run).

## Module-level constants (defined in Task 5, referenced throughout)

```python
RETENTION_RATE_MIN   = 0.85   # per-scenario retention axis passes at/above this
RETENTION_HARD_FLOOR = 0.70   # any scenario below this vetoes the whole run
VOICE_DRIFT_TOL      = 0.10    # last-third norm score may drop at most this vs first-third
LATENCY_CEILING_MS   = 8000.0  # calibration seed for TOTAL turn latency (31B 4-bit, recalibrate)
LATENCY_MAX_GROWTH   = 0.20    # last-third mean latency ≤ 20% over first-third mean
RUN_PASS_MIN_SCENARIOS = 5     # run passes if ≥5 of 6 scenarios pass (subject to hard floor)
```

## Scenario data structure (defined in Task 1, referenced throughout)

```python
{
    "name": str,            # matches an eval_multiturn.py scenario name
    "description": str,
    "anchors": [            # 3–5 per scenario
        {"turn": int, "fact": str, "probe_turn": int},
    ],
    "turns": [str, ...],    # 20–30 user messages (1-indexed positions referenced by anchors)
}
```

---

## Task 1: Deep scenario data module

**Files:**
- Create: `scripts/multiturn_scenarios_deep.py`
- Create (this task's tests): `scripts/test_eval_multiturn_local.py`

- [ ] **Step 1: Write the failing test (scenario structural validity)**

Create `scripts/test_eval_multiturn_local.py`:

```python
#!/usr/bin/env python3
"""Unit tests for eval_multiturn_local.py and multiturn_scenarios_deep.py.

Plain-runner pattern (no pytest). Run: python3 scripts/test_eval_multiturn_local.py
All model/judge I/O is mocked; no live mlx-server or ADC required.
"""
import json
import sys
import tempfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import multiturn_scenarios_deep as deep


def test_six_deep_scenarios_present():
    names = [s["name"] for s in deep.DEEP_SCENARIOS]
    assert len(deep.DEEP_SCENARIOS) == 6, f"expected 6 scenarios, got {len(names)}"
    expected = {"casual_catchup", "emotional_escalation", "debate_opinions",
                "banter_humor", "news_reaction_chain", "advice_seeking"}
    assert set(names) == expected, f"name mismatch: {set(names) ^ expected}"
    print("✓ six_deep_scenarios_present")


def test_each_scenario_has_20_to_30_turns():
    for s in deep.DEEP_SCENARIOS:
        n = len(s["turns"])
        assert 20 <= n <= 30, f"{s['name']}: {n} turns (want 20–30)"
    print("✓ each_scenario_has_20_to_30_turns")


def test_anchors_reference_valid_turn_indices():
    for s in deep.DEEP_SCENARIOS:
        n = len(s["turns"])
        assert 3 <= len(s["anchors"]) <= 5, f"{s['name']}: {len(s['anchors'])} anchors (want 3–5)"
        for a in s["anchors"]:
            assert 1 <= a["turn"] <= n, f"{s['name']}: anchor turn {a['turn']} out of range"
            assert 1 <= a["probe_turn"] <= n, f"{s['name']}: probe_turn {a['probe_turn']} out of range"
            assert a["probe_turn"] > a["turn"], (
                f"{s['name']}: probe_turn {a['probe_turn']} must come after fact turn {a['turn']}")
            assert a["fact"].strip(), f"{s['name']}: empty anchor fact"
    print("✓ anchors_reference_valid_turn_indices")
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'multiturn_scenarios_deep'`

- [ ] **Step 3: Create the data module (minimal — one full scenario, then fill the rest)**

Create `scripts/multiturn_scenarios_deep.py`. Author all 6 scenarios at 20–30 turns each, each in-character (continuation of the arc, never repeated prompts), with 3–5 anchors whose `probe_turn` naturally invites recall. Full worked example for `casual_catchup` (author the other 5 in the same shape):

```python
#!/usr/bin/env python3
"""Deep (20–30 turn) multi-turn scenarios with recallable anchors.

Data only — consumed by eval_multiturn_local.py. Each scenario extends a
short scenario from eval_multiturn.py to 20–30 user turns, in-character,
and plants 3–5 anchors: a fact stated at `turn`, recalled at `probe_turn`.
"""

DEEP_SCENARIOS = [
    {
        "name": "casual_catchup",
        "description": "Casual friend catching up over a long sitting",
        "anchors": [
            {"turn": 2,  "fact": "the user just adopted a dog named Biscuit", "probe_turn": 24},
            {"turn": 5,  "fact": "the user is flying to Denver on Friday",    "probe_turn": 19},
            {"turn": 8,  "fact": "the user hates cilantro",                   "probe_turn": 27},
            {"turn": 11, "fact": "the user's sister Mara is visiting next month", "probe_turn": 22},
        ],
        "turns": [
            "hey whats up",                                              # 1
            "not much, just got back from the shelter — adopted a dog! named him Biscuit",  # 2
            "haha yeah he's a menace already. anyway hbu",              # 3
            "oh nice. you been traveling at all lately?",               # 4
            "im actually flying to denver friday for a work thing",     # 5
            "yeah should be fun. you ever been?",                       # 6
            "cool cool. what should i eat while im out there",          # 7
            "ok but no cilantro on anything, i genuinely cant stand it",# 8
            "lol it tastes like soap to me, its a whole thing",         # 9
            "anyway what are you up to this weekend",                   # 10
            "nice. oh my sister mara is coming to visit next month btw",# 11
            "yeah we're gonna do the whole tourist thing",              # 12
            "you got any recs for stuff to do with her",                # 13
            "she's more into museums than bars honestly",               # 14
            "haha true. ok random but are you watching anything good",  # 15
            "i need a new show, just finished my last one",             # 16
            "ok ill check it out. how's work been for you",             # 17
            "ugh same. mondays are rough",                              # 18
            "so for denver — what should i pack? gonna be there 3 days",# 19  (probe: Denver/Friday)
            "good call. cold there this time of year?",                 # 20
            "noted. ok i should get going soon",                        # 21
            "oh before i forget — any ideas for where to take mara?",   # 22  (probe: sister Mara)
            "perfect, she'll love that",                                # 23
            "haha and i gotta get back before biscuit destroys the couch",  # 24  (probe: dog Biscuit)
            "he's lucky he's cute",                                     # 25
            "for real. ok last thing — dinner spot for tonight?",       # 26
            "as long as theres no cilantro im in",                      # 27  (probe: hates cilantro)
            "perfect. talk later man",                                  # 28
        ],
    },
    # TODO-FOR-AUTHOR: emotional_escalation, debate_opinions, banter_humor,
    # news_reaction_chain, advice_seeking — same structure, 20–30 turns, 3–5
    # anchors each with probe_turn > turn. Match each scenario's emotional arc
    # from eval_multiturn.py SCENARIOS (escalate the debate, keep banter
    # riffing, etc.). Do NOT pad with repeated prompts.
]
```

> NOTE TO IMPLEMENTER: The `TODO-FOR-AUTHOR` marker above is the ONE place this plan asks for genuine authoring judgment (writing natural in-character dialogue). Author the remaining 5 scenarios fully before moving on — the test in Step 1 enforces count=6, 20–30 turns, and 3–5 valid anchors each, so a stub will fail the test.

- [ ] **Step 4: Run the test to verify it passes**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: PASS — `✓ six_deep_scenarios_present`, `✓ each_scenario_has_20_to_30_turns`, `✓ anchors_reference_valid_turn_indices` (after all 6 scenarios authored)

- [ ] **Step 5: Commit**

```bash
git add scripts/multiturn_scenarios_deep.py scripts/test_eval_multiturn_local.py
git commit -m "feat(eval): deep 20-30 turn multi-turn scenarios with recallable anchors"
```

---

## Task 2: Latency math (pure functions)

**Files:**
- Create: `scripts/eval_multiturn_local.py`
- Modify: `scripts/test_eval_multiturn_local.py`

- [ ] **Step 1: Write the failing tests**

Append to `scripts/test_eval_multiturn_local.py`:

```python
import eval_multiturn_local as mt


def test_latency_ceiling_violations_flags_over_ceiling():
    series = [100.0, 200.0, 9000.0, 300.0, 12000.0]
    over = mt.latency_ceiling_violations(series, ceiling_ms=8000.0)
    assert over == [2, 4], f"expected indices [2,4], got {over}"
    print("✓ latency_ceiling_violations_flags_over_ceiling")


def test_latency_growth_flat_series_near_zero():
    series = [500.0] * 30
    g = mt.latency_growth(series)
    assert abs(g) < 1e-6, f"flat series should have ~0 growth, got {g}"
    print("✓ latency_growth_flat_series_near_zero")


def test_latency_growth_climbing_series_positive():
    # first third ~100, last third ~200 → growth ~1.0 (100%)
    series = [100.0] * 10 + [150.0] * 10 + [200.0] * 10
    g = mt.latency_growth(series)
    assert 0.9 < g < 1.1, f"expected ~1.0 growth, got {g}"
    print("✓ latency_growth_climbing_series_positive")


def test_latency_ok_passes_flat_under_ceiling():
    series = [400.0] * 24
    ok, detail = mt.latency_ok(series, ceiling_ms=8000.0, max_growth=0.20)
    assert ok is True, f"flat under-ceiling series should pass: {detail}"
    assert detail["ceiling_violations"] == []
    print("✓ latency_ok_passes_flat_under_ceiling")


def test_latency_ok_fails_on_growth():
    series = [100.0] * 12 + [500.0] * 12  # 400% growth
    ok, detail = mt.latency_ok(series, ceiling_ms=8000.0, max_growth=0.20)
    assert ok is False, "steeply climbing series should fail growth gate"
    assert detail["growth"] > 0.20
    print("✓ latency_ok_fails_on_growth")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'eval_multiturn_local'`

- [ ] **Step 3: Create the harness module with the latency functions**

Create `scripts/eval_multiturn_local.py`:

```python
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: PASS — all 5 latency tests print `✓`

- [ ] **Step 5: Commit**

```bash
git add scripts/eval_multiturn_local.py scripts/test_eval_multiturn_local.py
git commit -m "feat(eval): latency ceiling+growth gate for multi-turn coherence"
```

---

## Task 3: Retention scoring (pure functions)

**Files:**
- Modify: `scripts/eval_multiturn_local.py`
- Modify: `scripts/test_eval_multiturn_local.py`

- [ ] **Step 1: Write the failing tests**

Append to `scripts/test_eval_multiturn_local.py`:

```python
def test_retention_rate_all_retained():
    assert mt.retention_rate([True, True, True, True]) == 1.0
    print("✓ retention_rate_all_retained")


def test_retention_rate_none_retained():
    assert mt.retention_rate([False, False, False]) == 0.0
    print("✓ retention_rate_none_retained")


def test_retention_rate_partial():
    rate = mt.retention_rate([True, True, False, True])  # 3/4
    assert abs(rate - 0.75) < 1e-9, f"expected 0.75, got {rate}"
    print("✓ retention_rate_partial")


def test_retention_rate_empty_is_zero():
    assert mt.retention_rate([]) == 0.0
    print("✓ retention_rate_empty_is_zero")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: FAIL — `AttributeError: module 'eval_multiturn_local' has no attribute 'retention_rate'`

- [ ] **Step 3: Add the function**

Append to `scripts/eval_multiturn_local.py` (after `latency_ok`):

```python
def retention_rate(anchor_results):
    """Fraction of anchors the judge marked retained. Empty → 0.0."""
    if not anchor_results:
        return 0.0
    return sum(1 for r in anchor_results if r) / len(anchor_results)
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: PASS — 4 retention tests print `✓`

- [ ] **Step 5: Commit**

```bash
git add scripts/eval_multiturn_local.py scripts/test_eval_multiturn_local.py
git commit -m "feat(eval): per-anchor retention_rate scoring"
```

---

## Task 4: Voice-drift gate (pure functions)

**Files:**
- Modify: `scripts/eval_multiturn_local.py`
- Modify: `scripts/test_eval_multiturn_local.py`

- [ ] **Step 1: Write the failing tests**

Append to `scripts/test_eval_multiturn_local.py`:

```python
def test_voice_normalize_divides_by_ten():
    assert mt.voice_normalize(8.0) == 0.8
    assert mt.voice_normalize(10) == 1.0
    print("✓ voice_normalize_divides_by_ten")


def test_voice_drift_ok_stable_passes():
    # first-third 0.8, last-third 0.78 → drop 0.02 ≤ tol 0.10
    ok = mt.voice_drift_ok(0.8, 0.78, tol=0.10, any_hard_ai=False)
    assert ok is True
    print("✓ voice_drift_ok_stable_passes")


def test_voice_drift_ok_big_drop_fails():
    # drop 0.30 > tol 0.10
    ok = mt.voice_drift_ok(0.8, 0.5, tol=0.10, any_hard_ai=False)
    assert ok is False
    print("✓ voice_drift_ok_big_drop_fails")


def test_voice_drift_ok_hard_ai_late_fails():
    # small drop, but a late turn flipped to hard AI verdict
    ok = mt.voice_drift_ok(0.8, 0.79, tol=0.10, any_hard_ai=True)
    assert ok is False
    print("✓ voice_drift_ok_hard_ai_late_fails")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: FAIL — `AttributeError: ... has no attribute 'voice_normalize'`

- [ ] **Step 3: Add the functions**

Append to `scripts/eval_multiturn_local.py`:

```python
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: PASS — 4 voice tests print `✓`

- [ ] **Step 5: Commit**

```bash
git add scripts/eval_multiturn_local.py scripts/test_eval_multiturn_local.py
git commit -m "feat(eval): voice-drift-over-distance gate"
```

---

## Task 5: Verdict assembly + JSON shape (constants + pure functions)

**Files:**
- Modify: `scripts/eval_multiturn_local.py`
- Modify: `scripts/test_eval_multiturn_local.py`

- [ ] **Step 1: Write the failing tests**

Append to `scripts/test_eval_multiturn_local.py`:

```python
def test_scenario_verdict_all_axes_pass():
    sv = mt.scenario_verdict(
        name="casual_catchup",
        retention=0.90, voice_pass=True, voice_detail={"first": 0.8, "last": 0.79},
        latency_pass=True, latency_detail={"growth": 0.05, "ceiling_violations": []},
    )
    assert sv["passed"] is True
    assert sv["retention"]["passed"] is True
    assert sv["scenario"] == "casual_catchup"
    print("✓ scenario_verdict_all_axes_pass")


def test_scenario_verdict_retention_below_min_fails_axis():
    sv = mt.scenario_verdict(
        name="debate_opinions",
        retention=0.80, voice_pass=True, voice_detail={},
        latency_pass=True, latency_detail={},
    )
    assert sv["retention"]["passed"] is False  # 0.80 < RETENTION_RATE_MIN 0.85
    assert sv["passed"] is False
    print("✓ scenario_verdict_retention_below_min_fails_axis")


def test_run_verdict_five_of_six_passes():
    svs = [{"scenario": f"s{i}", "passed": (i != 5),
            "retention": {"rate": 0.9}} for i in range(6)]  # 5 pass, 1 fail, none below floor
    rv = mt.run_verdict(svs)
    assert rv["scenarios_passed"] == 5
    assert rv["run_passed"] is True
    assert rv["hard_floor_veto"] is False
    print("✓ run_verdict_five_of_six_passes")


def test_run_verdict_hard_floor_veto():
    svs = [{"scenario": f"s{i}", "passed": True,
            "retention": {"rate": 0.9}} for i in range(6)]
    svs[0]["retention"]["rate"] = 0.60  # below RETENTION_HARD_FLOOR 0.70
    rv = mt.run_verdict(svs)
    assert rv["hard_floor_veto"] is True
    assert rv["run_passed"] is False, "hard floor must veto even when 5/6 pass"
    print("✓ run_verdict_hard_floor_veto")


def test_write_verdict_roundtrip():
    verdict = {"run_passed": True, "scenarios": []}
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "verdict.json"
        mt.write_verdict(verdict, p)
        loaded = json.loads(p.read_text())
        assert loaded["run_passed"] is True
        assert "generated_at" in loaded
    print("✓ write_verdict_roundtrip")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: FAIL — `AttributeError: ... has no attribute 'scenario_verdict'`

- [ ] **Step 3: Add constants + assembly functions**

Insert the constants block near the top of `scripts/eval_multiturn_local.py` (right after the imports), then append the functions:

```python
# --- Thresholds (calibration seeds — Task 9 locks the real numbers) ---
RETENTION_RATE_MIN     = 0.85
RETENTION_HARD_FLOOR   = 0.70
VOICE_DRIFT_TOL        = 0.10
LATENCY_CEILING_MS     = 8000.0
LATENCY_MAX_GROWTH     = 0.20
RUN_PASS_MIN_SCENARIOS = 5
```

```python
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: PASS — 5 verdict tests print `✓`

- [ ] **Step 5: Commit**

```bash
git add scripts/eval_multiturn_local.py scripts/test_eval_multiturn_local.py
git commit -m "feat(eval): scenario + run verdict assembly with hard-floor veto"
```

---

## Task 6: LocalBackend HTTP client

**Files:**
- Modify: `scripts/eval_multiturn_local.py`
- Modify: `scripts/test_eval_multiturn_local.py`

- [ ] **Step 1: Write the failing tests (mock urlopen)**

Append to `scripts/test_eval_multiturn_local.py`:

```python
class _FakeResp:
    def __init__(self, payload):
        self._b = json.dumps(payload).encode()
    def read(self):
        return self._b
    def __enter__(self):
        return self
    def __exit__(self, *a):
        return False


def test_localbackend_chat_returns_content_and_latency():
    backend = mt.LocalBackend("http://127.0.0.1:8741")
    payload = {"choices": [{"message": {"content": "yo what's good"}}]}
    with mock.patch.object(mt.urllib.request, "urlopen", return_value=_FakeResp(payload)):
        content, latency_ms = backend.chat([{"role": "user", "content": "hey"}])
    assert content == "yo what's good"
    assert latency_ms >= 0.0
    print("✓ localbackend_chat_returns_content_and_latency")


def test_localbackend_sends_full_history():
    backend = mt.LocalBackend("http://127.0.0.1:8741")
    captured = {}

    def fake_urlopen(req, timeout=0):
        captured["body"] = json.loads(req.data)
        return _FakeResp({"choices": [{"message": {"content": "ok"}}]})

    history = [
        {"role": "user", "content": "turn1"},
        {"role": "assistant", "content": "reply1"},
        {"role": "user", "content": "turn2"},
    ]
    with mock.patch.object(mt.urllib.request, "urlopen", side_effect=fake_urlopen):
        backend.chat(history)
    assert captured["body"]["messages"] == history, "must send full accumulated history"
    print("✓ localbackend_sends_full_history")


def test_localbackend_unreachable_raises_backend_unreachable():
    backend = mt.LocalBackend("http://127.0.0.1:8741")
    with mock.patch.object(mt.urllib.request, "urlopen",
                           side_effect=OSError("connection refused")):
        try:
            backend.chat([{"role": "user", "content": "hey"}])
            assert False, "expected BackendUnreachable"
        except mt.BackendUnreachable:
            pass
    print("✓ localbackend_unreachable_raises_backend_unreachable")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: FAIL — `AttributeError: ... has no attribute 'LocalBackend'`

- [ ] **Step 3: Add the backend class + exception**

Append to `scripts/eval_multiturn_local.py`:

```python
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
```

Also add `import urllib.error` to the imports block at the top (next to `import urllib.request`).

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: PASS — 3 backend tests print `✓`

- [ ] **Step 5: Commit**

```bash
git add scripts/eval_multiturn_local.py scripts/test_eval_multiturn_local.py
git commit -m "feat(eval): LocalBackend HTTP client (full history, fail-fast, no cloud fallback)"
```

---

## Task 7: Judge wrappers (retention + voice windows)

**Files:**
- Modify: `scripts/eval_multiturn_local.py`
- Modify: `scripts/test_eval_multiturn_local.py`

- [ ] **Step 1: Write the failing tests (mock the Gemini judge)**

Append to `scripts/test_eval_multiturn_local.py`:

```python
def test_judge_anchor_retention_parses_true():
    with mock.patch.object(mt, "call_gemini", return_value='{"retained": true}'):
        out = mt.judge_anchor_retention("the dog is named Biscuit",
                                        "what about the dog",
                                        "Biscuit's doing great, total menace")
    assert out is True
    print("✓ judge_anchor_retention_parses_true")


def test_judge_anchor_retention_parses_false():
    with mock.patch.object(mt, "call_gemini", return_value='```json\n{"retained": false}\n```'):
        out = mt.judge_anchor_retention("flying to Denver Friday",
                                        "what should I pack",
                                        "pack for anything, who knows")
    assert out is False
    print("✓ judge_anchor_retention_parses_false")


def test_judge_available_true_when_token_present():
    with mock.patch.object(mt, "_get_adc_token", return_value="tok"):
        assert mt.judge_available() is True
    print("✓ judge_available_true_when_token_present")


def test_judge_available_false_when_no_token():
    with mock.patch.object(mt, "_get_adc_token", return_value=None):
        assert mt.judge_available() is False
    print("✓ judge_available_false_when_no_token")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: FAIL — `AttributeError: ... has no attribute 'judge_anchor_retention'`

- [ ] **Step 3: Add the judge wrappers (reuse eval_multiturn imports)**

Append to `scripts/eval_multiturn_local.py`. The import line goes in the imports block at the top:

```python
from eval_multiturn import (  # noqa: E402  (after sys.path.insert)
    call_gemini, _get_adc_token, evaluate_conversation, EVAL_DIMENSIONS,
)
```

Then the wrappers:

```python
def judge_available():
    """True when ADC credentials are present (judge can run)."""
    return _get_adc_token() is not None


def judge_anchor_retention(anchor_fact, probe_user, probe_response):
    """Ask the judge whether the probe-turn reply stayed consistent with an
    earlier-established fact. Returns bool. Raises on judge/ADC failure."""
    prompt = f"""A fact was established earlier in a text conversation:
  FACT: {anchor_fact}

Later, the friend said:
  FRIEND: {probe_user}
And the person ("Seth") replied:
  SETH: {probe_response}

Did Seth's reply remain CONSISTENT with the earlier fact (either by correctly
referencing it, or at minimum not contradicting it)? A reply that forgets or
contradicts the fact is NOT retained.

Return JSON: {{"retained": true|false, "why": "..."}}"""
    raw = call_gemini(prompt).strip()
    if raw.startswith("```"):
        raw = raw.split("\n", 1)[1].rsplit("```", 1)[0]
    return bool(json.loads(raw)["retained"])


def judge_voice_window(scenario_name, exchanges_window):
    """Score a window of (user, ai) exchanges. Returns (overall_score_1_10, verdict).

    Reuses eval_multiturn.evaluate_conversation. Returns (0.0, 'AI') on judge error.
    """
    result = evaluate_conversation(scenario_name, exchanges_window)
    if not result:
        return 0.0, "AI"
    return result.get("overall_score", 0.0), result.get("overall_verdict", "AI")
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: PASS — 4 judge tests print `✓`

- [ ] **Step 5: Commit**

```bash
git add scripts/eval_multiturn_local.py scripts/test_eval_multiturn_local.py
git commit -m "feat(eval): judge wrappers for anchor retention + voice windows"
```

---

## Task 8: Orchestration (run_scenario + main + exit codes)

**Files:**
- Modify: `scripts/eval_multiturn_local.py`
- Modify: `scripts/test_eval_multiturn_local.py`

- [ ] **Step 1: Write the failing tests (mock backend + judge end-to-end)**

Append to `scripts/test_eval_multiturn_local.py`:

```python
def _tiny_scenario():
    return {
        "name": "tiny",
        "description": "tiny test scenario",
        "anchors": [{"turn": 1, "fact": "user likes tea", "probe_turn": 3}],
        "turns": ["hi", "how are you", "what do I like to drink", "cool", "bye", "later"],
    }


def test_run_scenario_collects_latency_and_transcript():
    backend = mock.Mock()
    backend.chat.side_effect = [("r%d" % i, 100.0 + i) for i in range(6)]
    sv = mt.run_scenario(_tiny_scenario(), backend, judge_on=False)
    assert len(sv["latency"]["series_ms"]) == 6, "one latency sample per turn"
    # judge_on=False → retention/voice marked skipped, latency still computed
    assert sv["voice"]["passed"] in (True, None)
    print("✓ run_scenario_collects_latency_and_transcript")


def test_run_scenario_retention_uses_anchor_probe():
    backend = mock.Mock()
    backend.chat.side_effect = [("r%d" % i, 100.0) for i in range(6)]
    with mock.patch.object(mt, "judge_anchor_retention", return_value=True) as jar, \
         mock.patch.object(mt, "judge_voice_window", return_value=(8.0, "HUMAN")):
        sv = mt.run_scenario(_tiny_scenario(), backend, judge_on=True)
    assert jar.called, "retention judge must be invoked at probe turns"
    assert sv["retention"]["rate"] == 1.0
    print("✓ run_scenario_retention_uses_anchor_probe")


def test_main_writes_verdict_and_returns_exit_code():
    backend = mock.Mock()
    backend.chat.side_effect = [("reply", 100.0)] * 200  # plenty for all scenarios
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        with mock.patch.object(mt, "LocalBackend", return_value=backend), \
             mock.patch.object(mt, "judge_available", return_value=True), \
             mock.patch.object(mt, "judge_anchor_retention", return_value=True), \
             mock.patch.object(mt, "judge_voice_window", return_value=(9.0, "HUMAN")):
            code = mt.main(["--output-json", str(out), "--server-url", "http://x"])
        verdict = json.loads(out.read_text())
        assert "run_passed" in verdict and "scenarios" in verdict
        assert code in (0, 1)
    print("✓ main_writes_verdict_and_returns_exit_code")


def test_main_deferred_when_backend_unreachable():
    backend = mock.Mock()
    backend.chat.side_effect = mt.BackendUnreachable("refused")
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        with mock.patch.object(mt, "LocalBackend", return_value=backend), \
             mock.patch.object(mt, "judge_available", return_value=True):
            code = mt.main(["--output-json", str(out), "--server-url", "http://x"])
        assert code == 2, "unreachable backend → DEFERRED exit 2"
    print("✓ main_deferred_when_backend_unreachable")


def test_main_skipped_when_judge_unavailable():
    backend = mock.Mock()
    backend.chat.side_effect = [("reply", 100.0)] * 200
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        with mock.patch.object(mt, "LocalBackend", return_value=backend), \
             mock.patch.object(mt, "judge_available", return_value=False):
            code = mt.main(["--output-json", str(out), "--server-url", "http://x"])
        verdict = json.loads(out.read_text())
        assert code == 3, "judge unavailable → SKIPPED exit 3"
        assert verdict["judge"] == "SKIPPED"
    print("✓ main_skipped_when_judge_unavailable")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: FAIL — `AttributeError: ... has no attribute 'run_scenario'`

- [ ] **Step 3: Add run_scenario + main**

Append to `scripts/eval_multiturn_local.py`. Add `import multiturn_scenarios_deep` to imports.

```python
def run_scenario(scenario, backend, judge_on):
    """Drive one deep conversation, time each turn, score the three axes.

    Returns a scenario_verdict dict. When judge_on is False, retention/voice
    are marked skipped (passed=None) and only latency is gated.
    """
    messages = []
    exchanges = []          # (user, ai) per turn
    latency_series = []
    responses_by_turn = {}  # 1-indexed turn -> ai response

    for ti, user_msg in enumerate(scenario["turns"], start=1):
        messages.append({"role": "user", "content": user_msg})
        content, latency_ms = backend.chat(messages)   # may raise BackendUnreachable
        messages.append({"role": "assistant", "content": content})
        exchanges.append((user_msg, content))
        latency_series.append(latency_ms)
        responses_by_turn[ti] = content

    lat_ok, lat_detail = latency_ok(latency_series, LATENCY_CEILING_MS, LATENCY_MAX_GROWTH)

    if not judge_on:
        sv = scenario_verdict(
            name=scenario["name"], retention=0.0, voice_pass=None,
            voice_detail={"skipped": True}, latency_pass=lat_ok, latency_detail=lat_detail)
        sv["retention"]["skipped"] = True
        sv["passed"] = lat_ok  # only latency gates when judge is off
        return sv

    # Retention: judge each anchor at its probe turn.
    anchor_results = []
    for a in scenario["anchors"]:
        probe_user = scenario["turns"][a["probe_turn"] - 1]
        probe_resp = responses_by_turn[a["probe_turn"]]
        anchor_results.append(judge_anchor_retention(a["fact"], probe_user, probe_resp))
    rate = retention_rate(anchor_results)

    # Voice drift: judge first-third and last-third windows.
    first_ex, last_ex = _thirds(exchanges)
    first_score, _ = judge_voice_window(scenario["name"], first_ex)
    last_score, last_verdict = judge_voice_window(scenario["name"], last_ex)
    v_ok = voice_drift_ok(voice_normalize(first_score), voice_normalize(last_score),
                          VOICE_DRIFT_TOL, any_hard_ai=(last_verdict == "AI"))

    return scenario_verdict(
        name=scenario["name"], retention=rate, voice_pass=v_ok,
        voice_detail={"first_third_score": first_score, "last_third_score": last_score,
                      "last_third_verdict": last_verdict},
        latency_pass=lat_ok, latency_detail=lat_detail)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Sustained multi-turn coherence eval (on-device)")
    ap.add_argument("--server-url", default="http://127.0.0.1:8741")
    ap.add_argument("--output-json",
                    default=str(Path.home() / ".human" / "logs" / "eval-multiturn-local.json"))
    args = ap.parse_args(argv)

    backend = LocalBackend(args.server_url)
    judge_on = judge_available()

    scenario_verdicts = []
    try:
        for scenario in multiturn_scenarios_deep.DEEP_SCENARIOS:
            scenario_verdicts.append(run_scenario(scenario, backend, judge_on=judge_on))
    except BackendUnreachable as e:
        write_verdict({"run_passed": False, "backend": "UNREACHABLE", "error": str(e),
                       "scenarios": scenario_verdicts}, args.output_json)
        print(f"DEFERRED: mlx-server unreachable: {e}")
        return 2

    verdict = run_verdict(scenario_verdicts)
    verdict["judge"] = "OK" if judge_on else "SKIPPED"
    write_verdict(verdict, args.output_json)

    print(f"Run verdict: {'PASS' if verdict['run_passed'] else 'FAIL'} "
          f"({verdict['scenarios_passed']}/{verdict['scenarios_total']} scenarios) "
          f"judge={verdict['judge']}")

    if not judge_on:
        return 3  # SKIPPED — latency ran, qualitative axes did not
    return 0 if verdict["run_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 scripts/test_eval_multiturn_local.py`
Expected: PASS — all orchestration tests print `✓`, and the bottom-of-file `main()` runner reports `Results: N passed, 0 failed`

- [ ] **Step 5: Add the test runner to the bottom of the test file**

Ensure `scripts/test_eval_multiturn_local.py` ends with a runner that collects every `test_*` in module order (mirror `test_eval_fidelity_nightly.py`):

```python
def main():
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    passed = failed = 0
    for t in tests:
        try:
            t(); passed += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}"); failed += 1
        except Exception as e:
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}"); failed += 1
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 6: Commit**

```bash
git add scripts/eval_multiturn_local.py scripts/test_eval_multiturn_local.py
git commit -m "feat(eval): orchestrate multi-turn coherence run + verdict + exit codes"
```

---

## Task 9: First live run + threshold calibration

**Files:**
- Modify: `scripts/eval_multiturn_local.py` (constants only)
- Create: `docs/superpowers/specs/results/<date>-multiturn-local-verdict.json` (artifact, committed)

> This task is a MEASUREMENT, not unit-testable code. It requires the live mlx-server running on port 8741 and ADC credentials present. Do NOT skip — the seeds in Task 5 are guesses.

- [ ] **Step 1: Confirm the local mlx-server is up**

Run: `curl -s http://127.0.0.1:8741/v1/models -o /dev/null -w "%{http_code}\n"`
Expected: `200`. If not, the server is managed by launchd (`~/Library/LaunchAgents/ai.human.mlx-server.plist`); start it before continuing. (Per project rules: never `cp` over the running binary — use the install script if a rebuild is needed.)

- [ ] **Step 2: Run the full eval and capture the verdict**

Run:
```bash
python3 scripts/eval_multiturn_local.py \
  --output-json /tmp/multiturn-local-verdict.json
echo "exit=$?"
```
Expected: completes in minutes (6 scenarios × ~25 local generations + judge calls); prints a `Run verdict:` line; writes the JSON.

- [ ] **Step 3: Inspect the measured distributions**

Read `/tmp/multiturn-local-verdict.json`. For each axis, record the observed range across scenarios:
- retention rates (min/median/max)
- voice first-third vs last-third scores
- latency `series_ms` per scenario, the `growth` values, and any `ceiling_violations`

- [ ] **Step 4: Lock the thresholds to reality**

Edit the constants in `scripts/eval_multiturn_local.py`:
- `LATENCY_CEILING_MS`: set to a defensible ceiling above the observed clean-turn latency (e.g. the observed p95 of a healthy run + margin). The 8000 seed is almost certainly wrong for total turn latency.
- `LATENCY_MAX_GROWTH`: keep 0.20 unless the observed healthy-run growth is itself near/over 0.20 (then the metric is too noisy — widen and note why).
- `RETENTION_RATE_MIN` / `RETENTION_HARD_FLOOR` / `VOICE_DRIFT_TOL`: keep the design seeds unless the first run shows them obviously mis-placed; record the rationale for any change in a one-line comment next to the constant.

- [ ] **Step 5: Commit the calibrated thresholds + the first verdict artifact**

```bash
mkdir -p docs/superpowers/specs/results
cp /tmp/multiturn-local-verdict.json \
   docs/superpowers/specs/results/$(date +%Y-%m-%d)-multiturn-local-verdict.json
git add scripts/eval_multiturn_local.py \
        docs/superpowers/specs/results/*-multiturn-local-verdict.json
git commit -m "feat(eval): calibrate multi-turn thresholds from first live verdict"
```

> SECRET HYGIENE: the verdict JSON contains scenario transcripts only — it must NOT contain any content from `~/.human/config.json`. Eyeball the artifact before committing; the harness never reads that file, but confirm.

---

## Task 10: Spawn contingent remediation follow-ups

**Files:** none (this task spawns work, per the design's evidence-driven remediation)

- [ ] **Step 1: Map each failing axis to its remediation**

From the Task 9 verdict, for each axis that FAILED, spawn a follow-up using the design's decision table:

| Failed axis | Remediation follow-up to spawn |
|---|---|
| Retention (rate < min, or hard-floor veto) | Wire `compaction.c` into the turn flow (it exists but is not auto-triggered) — summarize oldest turns instead of the 20 KB hard truncation at `agent_turn.c:4823` |
| Latency (ceiling or growth) | Prompt-prefix reuse / KV-cache on the mlx-server, or compaction to cap prompt growth (`compatible.c` resends full history every turn) |
| Voice drift | Persona-anchor reinforcement in deep-turn prompt assembly (persona re-injection dilutes as history grows) |

- [ ] **Step 2: If all axes passed**

Record the PASS in memory (`~/.claude/projects/.../memory/`) as the rung-3 empirical proof (mirror the `m3_mission_validated.md` precedent), and note that no remediation is needed yet. Rung 3 is then proven; the harness becomes the regression guard for future model/adapter swaps.

---

## Self-Review

**1. Spec coverage:**
- Harness architecture (isolated `eval_multiturn_local.py`, imports judge from `eval_multiturn.py`, LocalBackend to port 8741) → Tasks 6, 7, 8 ✓
- Scenario extension + anchor seeding (6 deep scenarios, 3–5 anchors, probe turns) → Task 1 ✓
- Retention axis (countable + judge) → Tasks 3, 7, 8 ✓
- Voice-drift axis (first/last third, judge) → Tasks 4, 7, 8 ✓
- Latency axis (ceiling + growth, wall-clock) → Task 2, 8 ✓
- Overall verdict (all-axes per scenario, 5/6 run, hard-floor veto) → Task 5 ✓
- Verdict JSON artifact → Task 5 (`write_verdict`), Task 9 (committed artifact) ✓
- Error handling (unreachable fail-fast / no cloud fallback / ADC-absent SKIPPED / exit codes) → Tasks 6, 8 ✓
- Testing (pure functions, mocks, no live model/judge) → every task's tests ✓
- First verdict + calibration → Task 9 ✓
- Contingent remediation → Task 10 ✓

**2. Placeholder scan:** The only intentional author-judgment marker is `TODO-FOR-AUTHOR` in Task 1 Step 3 (authoring 5 natural dialogues) — flagged explicitly with an enforcing test, not a hidden gap. No "add error handling"/"write tests for the above"/"similar to Task N" placeholders; every code step shows complete code.

**3. Type consistency:** `LocalBackend.chat → (content, latency_ms)` used consistently (Tasks 6, 8). `scenario_verdict(...)` / `run_verdict(...)` keys (`retention.rate`, `retention.passed`, `voice.passed`, `latency.series_ms`, `passed`, `run_passed`, `scenarios_passed`, `hard_floor_veto`) match across Tasks 5 and 8 and the tests. `_thirds()` reused by latency (Task 2) and voice windows (Task 8). Constants (Task 5) referenced by name everywhere. `judge_anchor_retention`/`judge_voice_window`/`judge_available` signatures consistent across Tasks 7 and 8.

---

## Related

- Design spec: `docs/superpowers/specs/2026-05-28-sustained-multiturn-coherence-design.md`
- Pattern source: `scripts/eval_fidelity_nightly.py`, `scripts/test_eval_fidelity_nightly.py`
- Judge + base scenarios: `scripts/eval_multiturn.py`
- `.claude/rules/classifier-score-plus-flag-gate.md` (threshold provenance)
