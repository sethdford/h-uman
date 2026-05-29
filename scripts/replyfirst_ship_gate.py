#!/usr/bin/env python3
"""Combine fidelity + ordering verdicts into a single ship decision.

Applies THIS project's strict +0.22 fidelity floor (not the shared 0.05 nightly
floor) against the fidelity verdict's delta.mean, AND the ordering gate.

Exit: 0 = SHIP, 1 = NO-SHIP.
Run: python3 scripts/replyfirst_ship_gate.py --fidelity f.json --ordering o.json
"""
import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

FIDELITY_FLOOR = 0.22
ORDERING_FLOOR = 0.90
MAX_FIRST_IDX = 8


def ship_decision(fidelity_verdict: dict, ordering_verdict: dict,
                  fidelity_floor: float = FIDELITY_FLOOR,
                  ordering_floor: float = ORDERING_FLOOR,
                  max_idx: int = MAX_FIRST_IDX) -> dict:
    fid_delta = fidelity_verdict.get("delta", {}).get("mean", 0.0)
    fid_pass = fid_delta >= fidelity_floor
    pct = ordering_verdict.get("pct_reply_first", 0.0)
    median_idx = ordering_verdict.get("median_first_reply_token_idx", 1e9)
    ord_pass = pct >= ordering_floor and median_idx <= max_idx
    return {
        "timestamp": datetime.now().isoformat(),
        "ship": bool(fid_pass and ord_pass),
        "fidelity_pass": bool(fid_pass),
        "fidelity_delta": fid_delta,
        "fidelity_floor": fidelity_floor,
        "ordering_pass": bool(ord_pass),
        "pct_reply_first": pct,
        "median_first_reply_token_idx": median_idx,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fidelity", type=Path, required=True)
    ap.add_argument("--ordering", type=Path, required=True)
    ap.add_argument("--output-json", type=Path)
    args = ap.parse_args()

    d = ship_decision(json.loads(args.fidelity.read_text()),
                      json.loads(args.ordering.read_text()))
    print(json.dumps(d, indent=2))
    if args.output_json:
        args.output_json.write_text(json.dumps(d, indent=2))
    return 0 if d["ship"] else 1


if __name__ == "__main__":
    sys.exit(main())
