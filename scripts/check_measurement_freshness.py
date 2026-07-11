#!/usr/bin/env python3
"""Fail when the committed blind-A/B verdict is stale.

The real measurement can only run where the model serves (Seth's machine —
the launchd nightly), never on a bare CI runner. CI therefore audits the
EVIDENCE instead of re-running the work: this check fails when neither the
proxy tier (mode != ADVISORY) nor the human tier of
docs/evaluation/blind_ab_gate.json carries a timestamp within --max-age-days.

A dry-run refresh cannot satisfy it: dry runs stamp mode=ADVISORY, which is
excluded on purpose. See .github/workflows/evaluation.yml (blind-ab-gate job).

Exit codes: 0 fresh, 1 stale/unparseable (message names the fix), 2 usage.
"""

import argparse
import json
import sys
from datetime import datetime, timedelta

DEFAULT_GATE = "docs/evaluation/blind_ab_gate.json"
DEFAULT_MAX_AGE_DAYS = 14


def parse_ts(raw):
    """ISO-8601 timestamp -> datetime, or None."""
    if not raw:
        return None
    try:
        return datetime.fromisoformat(str(raw).replace("Z", "+00:00")).replace(tzinfo=None)
    except ValueError:
        return None


def freshest_real_measurement(gate):
    """Newest timestamp among tiers holding a REAL measurement.

    Proxy counts only when mode is not ADVISORY (dry runs write ADVISORY);
    human counts whenever it has scored ratings (n > 0).
    Returns (datetime|None, tier_name|None).
    """
    candidates = []
    proxy = gate.get("proxy") or {}
    if str(proxy.get("mode", "")).upper() != "ADVISORY":
        ts = parse_ts(proxy.get("timestamp"))
        if ts:
            candidates.append((ts, "proxy"))
    human = gate.get("human") or {}
    if (human.get("n") or 0) > 0:
        ts = parse_ts(human.get("timestamp"))
        if ts:
            candidates.append((ts, "human"))
    if not candidates:
        return None, None
    return max(candidates)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gate", default=DEFAULT_GATE)
    ap.add_argument("--max-age-days", type=int, default=DEFAULT_MAX_AGE_DAYS)
    ap.add_argument("--now", default=None, help="ISO timestamp override for tests")
    args = ap.parse_args(argv)

    try:
        with open(args.gate) as f:
            gate = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"FRESHNESS: FAIL — cannot read {args.gate}: {e}")
        return 1

    now = parse_ts(args.now) or datetime.now()
    ts, tier = freshest_real_measurement(gate)
    if ts is None:
        print("FRESHNESS: FAIL — no REAL measurement recorded in any tier "
              "(proxy is ADVISORY/absent, human has n=0). Run the local nightly "
              "gate (scripts/eval_blinded_ab.py --gate against live serving, or "
              "complete blind_ab ratings) and commit the updated gate JSON.")
        return 1

    age = now - ts
    limit = timedelta(days=args.max_age_days)
    if age > limit:
        print(f"FRESHNESS: FAIL — last real measurement ({tier} tier) is "
              f"{age.days} days old ({ts.isoformat()}), limit {args.max_age_days}d. "
              "CI cannot run this measurement (model serves locally); run the "
              "local nightly gate and commit the refreshed gate JSON.")
        return 1

    print(f"FRESHNESS: OK — {tier} tier measured {age.days}d ago ({ts.isoformat()})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
