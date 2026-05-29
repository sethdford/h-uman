#!/usr/bin/env python3
"""Unit test for train_replyfirst.assert_scale_2. Stdlib only, no model.
Run: python3 scripts/test_train_replyfirst.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import train_replyfirst as tr


def test_scale_2_passes():
    tr.assert_scale_2({"lora_parameters": {"scale": 2.0, "rank": 8}})  # no raise
    print("✓ scale_2_passes")


def test_scale_20_hard_fails():
    try:
        tr.assert_scale_2({"lora_parameters": {"scale": 20.0}})
    except SystemExit as e:
        assert "2.0" in str(e), str(e)
        print("✓ scale_20_hard_fails"); return
    raise AssertionError("expected SystemExit on scale=20.0")


def test_missing_scale_hard_fails():
    try:
        tr.assert_scale_2({"lora_parameters": {}})
    except SystemExit:
        print("✓ missing_scale_hard_fails"); return
    raise AssertionError("expected SystemExit on missing scale")


def run():
    test_scale_2_passes()
    test_scale_20_hard_fails()
    test_missing_scale_hard_fails()
    print("\nALL train_replyfirst TESTS PASSED")


if __name__ == "__main__":
    run()
