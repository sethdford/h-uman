#!/usr/bin/env python3
"""Unit tests for scripts/check_capability_gates.py (stdlib runner)."""
import os, sys, json, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_capability_gates as c


def _write(d, registry, gate):
    rp = os.path.join(d, "reg.json"); gp = os.path.join(d, "gate.json")
    json.dump(registry, open(rp, "w")); json.dump(gate, open(gp, "w"))
    return rp, gp


def test_live_with_red_gate_fails():
    with tempfile.TemporaryDirectory() as d:
        rp, gp = _write(d,
            {"capabilities": [{"id": "x", "env": "X", "state": "LIVE", "required_gate": "pass"}]},
            {"effective_verdict": "ADVISORY"})
        assert c.check(rp, gp) != 0


def test_live_with_green_gate_passes():
    with tempfile.TemporaryDirectory() as d:
        rp, gp = _write(d,
            {"capabilities": [{"id": "x", "env": "X", "state": "LIVE", "required_gate": "pass"}]},
            {"effective_verdict": "PASS",
             "human": {"verdict": "PASS", "n": 30}})
        assert c.check(rp, gp) == 0


def test_live_with_human_absent_fails():
    """Wave B: LIVE + human ABSENT must fail closed even if effective looks green."""
    with tempfile.TemporaryDirectory() as d:
        rp, gp = _write(d,
            {"capabilities": [{"id": "x", "env": "X", "state": "LIVE", "required_gate": "pass"}]},
            {"effective_verdict": "PASS",
             "human": {"verdict": "ABSENT", "n": 0}})
        assert c.check(rp, gp) != 0


def test_non_live_with_red_gate_passes():
    with tempfile.TemporaryDirectory() as d:
        rp, gp = _write(d,
            {"capabilities": [{"id": "x", "env": "X", "state": "SHADOW", "required_gate": "pass"}]},
            {"effective_verdict": "FAIL"})
        assert c.check(rp, gp) == 0


def test_live_with_missing_gate_fails_closed():
    with tempfile.TemporaryDirectory() as d:
        rp = os.path.join(d, "reg.json")
        json.dump({"capabilities": [{"id": "x", "env": "X", "state": "LIVE", "required_gate": "pass"}]}, open(rp, "w"))
        assert c.check(rp, os.path.join(d, "does_not_exist.json")) != 0


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
