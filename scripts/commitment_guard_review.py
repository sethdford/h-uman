#!/usr/bin/env python3
"""Owner review of HU_COMMITMENT_GUARD shadow decisions (local only).

The shadow line never carries text. In SHADOW the draft is what was sent, so
the reply sits in chat.db at the line's timestamp. This script pairs each
detected shadow event (kind != none) with Seth's first outbound message in the
following --window seconds, shows it ON THIS TERMINAL ONLY, and asks: "was this
really a commitment of that kind?" It prints precision per kind and writes
nothing but the aggregate tally (--out, optional). No text is stored.

Run it yourself, on the Mac that holds chat.db (Full Disk Access for the
terminal). Docs: docs/guides/commitment-guard.md.

    python3 scripts/commitment_guard_review.py --limit 30
"""
import argparse
import glob
import json
import os
import re
import sqlite3
import sys
from datetime import datetime

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
from imessage_text import decode_attributed_body  # noqa: E402

APPLE_EPOCH = 978307200
LINE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}).*\[HU_COMMITMENT_GUARD shadow\] (?P<kv>.*)$"
)


def parse_events(lines):
    """Shadow lines where the detector found a commitment, oldest first."""
    out = []
    for line in lines:
        m = LINE.match(line.rstrip("\n"))
        if not m:
            continue
        kv = dict(p.split("=", 1) for p in m.group("kv").split() if "=" in p)
        if kv.get("kind", "none") == "none" or kv.get("detector") != "ok":
            continue
        ts = datetime.strptime(m.group("ts"), "%Y-%m-%dT%H:%M:%S").timestamp()
        out.append({"ts": ts, "kind": kv["kind"], "decision": kv.get("decision", "?"),
                    "calendar": kv.get("calendar", "?"), "audit": kv.get("audit") == "1"})
    return out


def sent_after(db, ts, window):
    """Seth's first outbound message in [ts, ts + window], or None."""
    lo = int((ts - APPLE_EPOCH) * 1e9)
    hi = int((ts + window - APPLE_EPOCH) * 1e9)
    row = db.execute(
        "SELECT text, attributedBody FROM message WHERE is_from_me = 1 AND date BETWEEN ? AND ? "
        "ORDER BY date LIMIT 1", (lo, hi)).fetchone()
    if not row:
        return None
    return row[0] or (decode_attributed_body(row[1]) if row[1] else None)


def tally(labels):
    """{kind: {"yes": n, "no": n, "precision": p | None}} plus an "all" row."""
    out = {}
    for kind, ok in labels:
        for k in (kind, "all"):
            row = out.setdefault(k, {"yes": 0, "no": 0})
            row["yes" if ok else "no"] += 1
    for row in out.values():
        n = row["yes"] + row["no"]
        row["precision"] = round(row["yes"] / n, 3) if n else None  # never 0 on n=0
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--log", action="append",
                    help="daemon log(s); default ~/.human/logs/service-loop*.log")
    ap.add_argument("--chatdb", default=os.environ.get("HU_CHATDB")
                    or os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--window", type=int, default=180)
    ap.add_argument("--limit", type=int, default=30)
    ap.add_argument("--answers", help="non-interactive y/n string (tests)")
    ap.add_argument("--out", help="write the aggregate tally JSON here")
    a = ap.parse_args(argv)

    logs = a.log or sorted(glob.glob(os.path.expanduser("~/.human/logs/service-loop*.log")))
    lines = []
    for path in logs:
        with open(path, errors="replace") as f:
            lines.extend(f)
    events = parse_events(lines)[-a.limit:]
    if not events:
        print("no detected shadow events yet: nothing to review", file=sys.stderr)
        return 2
    db = sqlite3.connect(f"file:{a.chatdb}?mode=ro", uri=True)
    answers = iter(a.answers or "")
    labels, unmatched = [], 0
    for i, ev in enumerate(events, 1):
        text = sent_after(db, ev["ts"], a.window)
        if text is None:
            unmatched += 1
            continue
        print(f"\n[{i}/{len(events)}] kind={ev['kind']} decision={ev['decision']} "
              f"calendar={ev['calendar']}\n  sent: {text}")
        ans = next(answers, None) if a.answers is not None else \
            input(f"  really a {ev['kind']} commitment? [y/n/s=skip] ").strip().lower()
        if ans in ("y", "n"):
            labels.append((ev["kind"], ans == "y"))
    db.close()
    result = {"events": len(events), "unmatched": unmatched, "labelled": len(labels),
              "by_kind": tally(labels)}
    print("\n" + json.dumps(result, indent=2))
    if a.out:
        with open(a.out, "w") as f:
            json.dump(result, f, indent=2)
    return 0 if labels else 2


if __name__ == "__main__":
    sys.exit(main())
