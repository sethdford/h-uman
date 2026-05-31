#!/usr/bin/env python3
"""CI registry check: any capability LIVE must have a green blind_ab gate.

Fail-closed: a LIVE capability with a missing/non-PASS gate fails CI. This is
the enforcement that makes feature-gate-requires-measurement real.
"""
import json
import os
import sys

_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_REGISTRY = os.path.join(_ROOT, "docs", "evaluation", "capability_gates.json")
DEFAULT_GATE = os.path.join(_ROOT, "docs", "evaluation", "blind_ab_gate.json")


def check(registry_path=DEFAULT_REGISTRY, gate_path=DEFAULT_GATE):
    """Return 0 if all LIVE capabilities have a PASS gate, else nonzero."""
    try:
        reg = json.load(open(registry_path))
    except (OSError, ValueError) as e:
        print(f"ERROR: cannot read registry {registry_path}: {e}")
        return 2
    live = [cap for cap in reg.get("capabilities", [])
            if cap.get("state") == "LIVE" and cap.get("required_gate") == "pass"]
    if not live:
        print("Capability gate check: no LIVE capabilities require the gate — OK")
        return 0
    try:
        gate = json.load(open(gate_path))
        effective = gate.get("effective_verdict")
    except (OSError, ValueError):
        effective = None  # fail closed
    if effective != "PASS":
        for cap in live:
            print(f"FAIL: capability '{cap['id']}' is LIVE but blind_ab gate "
                  f"effective_verdict={effective!r} (need PASS)")
        return 1
    print(f"Capability gate check: {len(live)} LIVE capabilities, gate PASS — OK")
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    sys.exit(check(*args) if args else check())
