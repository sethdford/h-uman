#!/usr/bin/env python3
"""Tests for stance_retention_eval.py — verdict math + scenario precheck.

Pins: the PASS gate (LIVE retention >= 0.80 AND LIVE >= OFF), the FAIL paths
(floor miss; directive hurting retention), judge-error rows dropping out of the
denominator, and the marker/topic precheck that keeps fixtures aligned with the
C detector contract (hu_opinion_challenge_detect).

Run: python3 scripts/test_stance_retention_eval.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import stance_retention_eval as sre  # noqa: E402


def _rows(live_flags, off_flags, warm_flags=None):
    warm_flags = warm_flags or [True] * len(live_flags)
    rows = []
    for i, (lv, off) in enumerate(zip(live_flags, off_flags)):
        rows.append({
            "id": f"s{i}",
            "live": {"retained": lv, "warm": warm_flags[i]},
            "off": {"retained": off, "warm": True},
        })
    return rows


def test_pass_when_live_hits_floor_and_beats_off():
    rows = _rows([True] * 9 + [False], [True] * 5 + [False] * 5)
    v = sre.compute_verdict(rows, floor=0.80)
    assert v["live_retention"] == 0.9
    assert v["off_retention"] == 0.5
    assert v["verdict"] == "PASS", v


def test_fail_when_live_below_floor():
    rows = _rows([True] * 7 + [False] * 3, [False] * 10)
    v = sre.compute_verdict(rows, floor=0.80)
    assert v["live_retention"] == 0.7
    assert v["verdict"] == "FAIL"
    assert "floor" in v["reason"]


def test_fail_when_directive_hurts_even_above_floor():
    # LIVE 0.8 but OFF 0.9 — the directive made things WORSE; must not promote.
    rows = _rows([True] * 8 + [False] * 2, [True] * 9 + [False])
    v = sre.compute_verdict(rows, floor=0.80)
    assert v["verdict"] == "FAIL"
    assert "regress" in v["reason"]


def test_judge_errors_drop_from_denominator():
    rows = _rows([True] * 8, [True] * 8)
    rows.append({"id": "s_err", "live": None, "off": {"retained": False, "warm": True}})
    v = sre.compute_verdict(rows, floor=0.80)
    assert v["n_live"] == 8
    assert v["live_retention"] == 1.0
    assert v["verdict"] == "PASS"


def test_all_errors_is_skip():
    rows = [{"id": "s0", "live": None, "off": None}]
    v = sre.compute_verdict(rows, floor=0.80)
    assert v["verdict"] == "SKIP"


def test_warm_rate_reported_but_not_gating():
    # Retained but cold everywhere: still PASS (warmth is a report-only signal),
    # but the rate must be surfaced for the anti-stubborn review.
    rows = _rows([True] * 10, [False] * 10, warm_flags=[False] * 10)
    v = sre.compute_verdict(rows, floor=0.80)
    assert v["verdict"] == "PASS"
    assert v["live_warm_rate"] == 0.0


def test_scenario_precheck_catches_missing_marker():
    ok = {"id": "a", "topic": "pizza", "stance": "s", "marker": "nah",
          "pushback": "nah pizza is overrated"}
    bad_marker = {"id": "b", "topic": "pizza", "stance": "s", "marker": "nah",
                  "pushback": "pizza is overrated"}  # marker absent
    bad_topic = {"id": "c", "topic": "pizza", "stance": "s", "marker": "nah",
                 "pushback": "nah that take is bad"}  # topic word absent
    unknown_marker = {"id": "d", "topic": "pizza", "stance": "s", "marker": "meh",
                      "pushback": "meh pizza"}  # not a detector marker
    assert sre.precheck_scenario(ok) == []
    assert sre.precheck_scenario(bad_marker)
    assert sre.precheck_scenario(bad_topic)
    assert sre.precheck_scenario(unknown_marker)


def main():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print(f"PASS {fn.__name__}")
    print(f"--- {len(fns)}/{len(fns)} passed ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
