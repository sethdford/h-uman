#!/usr/bin/env python3
"""HU_LENGTH_POLICY SHADOW measurement (docs/guides/length-policy.md).

Summarizes the daemon's "[HU_LENGTH_POLICY shadow|live]" log lines: per 1:1
turn with measured reply stats, today's cap (old_cap) against the policy's
(new_cap), and how often each wording says "keep it tight". The lines carry
only caps, flags, byte lengths and enums, so this reads no message text and
prints no contact. Refuses (exit 2, no verdict) when no turn had stats.

  python3 scripts/length_policy_caps.py [--log ~/.human/logs/service-loop-error.log]
"""
import argparse
import json
import os
import re
import statistics
import sys

LOG_RE = re.compile(
    r"\[HU_LENGTH_POLICY (shadow|live)\] old_cap=(\d+) new_cap=(\d+) "
    r"tight_old=(\d) tight_new=(\d) stats=(\d)")


def summarize(lines):
    rows = [m.groups() for m in map(LOG_RE.search, lines) if m]
    with_stats = [tuple(int(x) for x in r[1:]) for r in rows if r[5] == "1"]
    out = {"turns": len(rows), "turns_with_stats": len(with_stats)}
    if not with_stats:
        return out
    old = [r[0] for r in with_stats]
    new = [r[1] for r in with_stats]
    out.update({
        "old_cap_median": statistics.median(old),
        "new_cap_median": statistics.median(new),
        "new_gt_old": sum(1 for o, n in zip(old, new) if n > o),
        "new_lt_old": sum(1 for o, n in zip(old, new) if n < o),
        "tight_old_rate": sum(r[2] for r in with_stats) / len(with_stats),
        "tight_new_rate": sum(r[3] for r in with_stats) / len(with_stats),
    })
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--log", default=os.path.expanduser("~/.human/logs/service-loop-error.log"))
    a = ap.parse_args(argv)
    with open(a.log, errors="replace") as f:
        out = summarize(f)
    print(json.dumps(out, indent=2))
    if not out["turns_with_stats"]:
        print("refused: no [HU_LENGTH_POLICY] turn with contact stats in the log",
              file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
