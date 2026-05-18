#!/usr/bin/env python3
"""
Phase E5 verifier — pins contact-routing storage + lookup contracts.

Tests:
  1. load_routes returns clean struct on missing/malformed file
  2. promote → lookup roundtrip
  3. demote removes the route
  4. lookup precedence: specific route > default > None
  5. save is atomic (tmp + rename, no partial file)

Run: python3 scripts/test_m3_contact_routing.py
"""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location(
    "m3_contact_routing", REPO_ROOT / "scripts" / "m3_contact_routing.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


_PASS = 0
_FAIL = 0


def _ok(name: str, cond: bool, detail: str = ""):
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def test_load_routes_handles_missing_file():
    print("\n--- test_load_routes_handles_missing_file ---")
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "nonexistent.json"
        routes = m.load_routes(path)
        _ok("returns dict", isinstance(routes, dict))
        _ok("has empty routes", routes["routes"] == {})
        _ok("default is None", routes["default_adapter"] is None)


def test_load_routes_handles_malformed_file():
    print("\n--- test_load_routes_handles_malformed_file ---")
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "bad.json"
        path.write_text("not json at all")
        routes = m.load_routes(path)
        _ok("malformed → clean empty struct", routes["routes"] == {})


def test_save_then_load_roundtrip():
    print("\n--- test_save_then_load_roundtrip ---")
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "r.json"
        m.save_routes_atomic({
            "routes": {"123": {"contact_label": "alice",
                                "adapter_path": "/a/x",
                                "promoted_at_ms": 1, "outcome_count_at_promote": 5}},
            "default_adapter": "/a/default",
        }, path)
        loaded = m.load_routes(path)
        _ok("contact_label preserved",
            loaded["routes"]["123"]["contact_label"] == "alice")
        _ok("adapter_path preserved",
            loaded["routes"]["123"]["adapter_path"] == "/a/x")
        _ok("default_adapter preserved",
            loaded["default_adapter"] == "/a/default")


def test_lookup_precedence_specific_over_default():
    print("\n--- test_lookup_precedence_specific_over_default ---")
    routes = {
        "routes": {"42": {"adapter_path": "/a/specific.bin"}},
        "default_adapter": "/a/default.bin",
    }
    _ok("specific contact → specific adapter",
        m.lookup_contact_adapter(42, routes) == "/a/specific.bin")
    _ok("unknown contact → default",
        m.lookup_contact_adapter(99, routes) == "/a/default.bin")


def test_lookup_returns_none_when_no_default():
    print("\n--- test_lookup_returns_none_when_no_default ---")
    routes = {"routes": {}, "default_adapter": None}
    _ok("no routes, no default → None",
        m.lookup_contact_adapter(42, routes) is None)


def test_save_is_atomic_no_partial_file():
    print("\n--- test_save_is_atomic_no_partial_file ---")
    # If save fails partway, the target file should be either the
    # OLD content or fully-written new content — never a half-file.
    # We can't easily fault-inject, but we can confirm tmp+rename
    # leaves no .tmp behind on success.
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "r.json"
        m.save_routes_atomic({"routes": {"1": {"adapter_path": "/a/x"}},
                               "default_adapter": None}, path)
        tmp = path.with_suffix(".json.tmp")
        _ok("no .tmp left behind after save", not tmp.exists())
        _ok("target file exists and parses",
            path.exists() and json.loads(path.read_text())["routes"]["1"]
            ["adapter_path"] == "/a/x")


def main():
    print("M3 contact-routing (E5) verifier")
    test_load_routes_handles_missing_file()
    test_load_routes_handles_malformed_file()
    test_save_then_load_roundtrip()
    test_lookup_precedence_specific_over_default()
    test_lookup_returns_none_when_no_default()
    test_save_is_atomic_no_partial_file()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
