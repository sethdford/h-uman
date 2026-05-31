#!/usr/bin/env python3
"""Unit tests for scripts/blind_ab_gate.py (stdlib runner — no pytest dep)."""
import os, sys, json, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blind_ab_gate as g


def test_effective_human_fail_vetoes_proxy_pass():
    proxy = {"verdict": "PASS", "mode": "ENFORCING"}
    human = {"verdict": "FAIL"}
    assert g.compute_effective_verdict(proxy, human) == "FAIL"


def test_effective_both_pass():
    assert g.compute_effective_verdict(
        {"verdict": "PASS", "mode": "ENFORCING"}, {"verdict": "PASS"}) == "PASS"


def test_effective_proxy_enforcing_fail_no_human():
    assert g.compute_effective_verdict(
        {"verdict": "FAIL", "mode": "ENFORCING"}, {"verdict": "ABSENT"}) == "FAIL"


def test_effective_advisory_when_proxy_advisory_no_human():
    assert g.compute_effective_verdict(
        {"verdict": "ADVISORY", "mode": "ADVISORY"}, {"verdict": "ABSENT"}) == "ADVISORY"


def test_effective_human_stale_does_not_pass_on_its_own():
    assert g.compute_effective_verdict(
        {"verdict": "ADVISORY", "mode": "ADVISORY"}, {"verdict": "STALE"}) == "ADVISORY"


def test_proxy_decision_advisory_below_threshold():
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=10.0, n_real_pairs=5, baseline=None,
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert mode == "ADVISORY" and verdict == "ADVISORY" and fail is False


def test_proxy_decision_enforcing_pass():
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=50.0, n_real_pairs=40, baseline=None,
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert mode == "ENFORCING" and verdict == "PASS" and fail is False


def test_proxy_decision_enforcing_fail_under_floor():
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=40.0, n_real_pairs=40, baseline=None,
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert mode == "ENFORCING" and verdict == "FAIL" and fail is True


def test_proxy_decision_enforcing_fail_on_regression():
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=46.0, n_real_pairs=40, baseline={"fool_rate": 55.0},
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert verdict == "FAIL" and fail is True  # 55 - 46 = 9 > 5


def test_merge_preserves_other_half():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "gate.json")
        g.write_proxy_half(p, {"fool_rate": 50.0, "mode": "ENFORCING",
                               "verdict": "PASS", "n_real_pairs": 40,
                               "n_trials": 40, "baseline_fool_rate": None,
                               "fail_under": 45, "max_regression": 5}, commit="abc")
        g.write_human_half(p, {"detection": 0.5, "ci_lo": 0.4, "n": 30,
                               "verdict": "PASS"})
        data = json.load(open(p))
        assert data["proxy"]["fool_rate"] == 50.0
        assert data["human"]["verdict"] == "PASS"
        assert data["effective_verdict"] == "PASS"


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for fn in fns:
        try:
            fn(); print(f"PASS {fn.__name__}")
        except Exception as e:
            failed += 1; print(f"FAIL {fn.__name__}: {e}")
    print(f"\n{len(fns)-failed}/{len(fns)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    _run()
