#!/usr/bin/env python3
"""iMessage reply latency, measured from the service log.

Pairs every successful agent turn with the LAST inbound message from the same
handle in the 15 minutes before it and the FIRST send in the 10 minutes after
it, then reports p50/p90/max for three spans: inbound -> reply generated,
reply generated -> first send, inbound -> first send. The owner's own handles
(self-tests) are reported separately so they never skew real-contact numbers.

Usage: scripts/imessage_reply_latency.py [--since YYYY-MM-DD[THH:MM]] [--until ...] [--log PATH]
Exit 0 with a report; exit 2 when there are no paired real-contact turns
(a number with n=0 is not a measurement).
"""
import argparse
import datetime
import os
import re
import statistics
import sys

OWNER_HANDLES = {"+18012017497", "sethdouglasford@gmail.com", "+14845661687"}


def log_tag(handle):
    """The daemon logs handles as a 16-bit FNV-1a tag ("#3fa2"; src/core/log_redact.c)."""
    h = 2166136261
    for b in handle.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return "#%04x" % ((h ^ (h >> 16)) & 0xFFFF)


# Raw handles too, for logs from before 2026-10-02 or HU_LOG_CONTENT=1 runs.
OWNER = OWNER_HANDLES | {log_tag(h) for h in OWNER_HANDLES}


def parse_ts(s):
    for fmt in ("%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M", "%Y-%m-%d"):
        try:
            return datetime.datetime.strptime(s, fmt)
        except ValueError:
            pass
    raise SystemExit(f"bad --since: {s}")


def load_events(path, since, until):
    ev = []
    for line in open(path, errors="replace"):
        m = re.match(r"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)", line)
        if not m:
            continue
        t = datetime.datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%S")
        if t < since or (until and t >= until):
            continue
        if (m2 := re.search(r"incoming handle=(\S+)", line)):
            ev.append((t, "in", m2.group(1)))
        elif (m3 := re.search(r"agent turn result: err=ok response_len=\d+ for (\S+)", line)):
            ev.append((t, "turn", m3.group(1)))
        elif "imessage_dispatch:" in line or "voice delivered via Messages" in line:
            ev.append((t, "send", None))
    return ev


def pair(ev):
    rows = []
    for i, (t, kind, h) in enumerate(ev):
        if kind != "turn":
            continue
        ins = [e[0] for e in ev[:i] if e[1] == "in" and e[2] == h and (t - e[0]).total_seconds() < 900]
        sends = [e[0] for e in ev[i + 1:] if e[1] == "send" and (e[0] - t).total_seconds() < 600]
        if ins and sends:
            rows.append((h, ins[-1], t, sends[0]))
    return rows


def q(xs):
    xs = sorted(xs)
    return "p50=%.0fs p90=%.0fs max=%.0fs" % (statistics.median(xs), xs[int(0.9 * (len(xs) - 1))], xs[-1])


def report(name, rows):
    if not rows:
        print(f"{name}: n=0")
        return
    print(f"{name}: n={len(rows)}")
    print("  inbound -> reply generated ", q([(r[2] - r[1]).total_seconds() for r in rows]))
    print("  reply generated -> send    ", q([(r[3] - r[2]).total_seconds() for r in rows]))
    print("  inbound -> send            ", q([(r[3] - r[1]).total_seconds() for r in rows]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--since", default=None)
    ap.add_argument("--until", default=None)
    ap.add_argument("--log", default=os.path.expanduser("~/.human/logs/service-loop-error.log"))
    a = ap.parse_args()
    since = parse_ts(a.since) if a.since else datetime.datetime.now() - datetime.timedelta(days=1)
    until = parse_ts(a.until) if a.until else None
    rows = pair(load_events(a.log, since, until))
    real = [r for r in rows if r[0] not in OWNER]
    report(f"real contacts since {since:%Y-%m-%d %H:%M}", real)
    report("owner self-tests", [r for r in rows if r[0] in OWNER])
    return 0 if real else 2


if __name__ == "__main__":
    sys.exit(main())
