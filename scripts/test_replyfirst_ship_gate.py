#!/usr/bin/env python3
"""Unit tests for replyfirst_ship_gate. Stdlib only.
Run: python3 scripts/test_replyfirst_ship_gate.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import replyfirst_ship_gate as sg


def _fid(delta):  # minimal fidelity verdict
    return {"delta": {"mean": delta}, "verdict": "PASS"}


def _ord(pct, idx):
    return {"pct_reply_first": pct, "median_first_reply_token_idx": idx}


def test_ship_true_when_both_pass():
    d = sg.ship_decision(_fid(0.25), _ord(0.95, 0))
    assert d["ship"] is True and d["fidelity_pass"] and d["ordering_pass"], d
    print("✓ ship_true_when_both_pass")


def test_no_ship_when_fidelity_below_022():
    d = sg.ship_decision(_fid(0.18), _ord(0.95, 0))
    assert d["ship"] is False and d["fidelity_pass"] is False, d
    print("✓ no_ship_when_fidelity_below_022")


def test_no_ship_when_ordering_below_090():
    d = sg.ship_decision(_fid(0.25), _ord(0.80, 0))
    assert d["ship"] is False and d["ordering_pass"] is False, d
    print("✓ no_ship_when_ordering_below_090")


def test_no_ship_when_median_idx_too_high():
    d = sg.ship_decision(_fid(0.25), _ord(0.95, 12))
    assert d["ship"] is False and d["ordering_pass"] is False, d
    print("✓ no_ship_when_median_idx_too_high")


def run():
    test_ship_true_when_both_pass()
    test_no_ship_when_fidelity_below_022()
    test_no_ship_when_ordering_below_090()
    test_no_ship_when_median_idx_too_high()
    print("\nALL replyfirst_ship_gate TESTS PASSED")


if __name__ == "__main__":
    run()
