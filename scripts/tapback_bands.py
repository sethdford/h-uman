#!/usr/bin/env python3
"""Measure Seth's OWN tapback behavior from chat.db (read-only) — roadmap #18.

Computes, per contact and overall:
  - rate:   my tapbacks sent / messages received from that contact
  - latency: tapback.date - target message.date (p50/p90, ms)

Writes ~/.human/tapback_bands.json, consumed by src/behavior/tapback_band.c
(hu_tapback_band_load) to drop stale tapbacks on the daemon dispatch path.

chat.db notes:
  - message.date is ns since 2001-01-01 on modern macOS (seconds pre-High
    Sierra); latency subtracts same-epoch values so no epoch shift is needed.
  - Tapback rows: is_from_me=1, associated_message_type 2000-2005.
  - associated_message_guid may carry a "p:0/" or "bp:" prefix before the
    target GUID — strip through the last '/'.

Usage: tapback_bands.py [--db PATH] [--out PATH] [--min-n 5] [--dry-run]
"""

import argparse
import datetime
import json
import os
import sqlite3
import sys

APPLE_NS_THRESHOLD = 1_000_000_000_000  # values above this are ns, below are s

LATENCY_SQL = """
SELECT h.id AS contact, t.date AS tap_date, m.date AS target_date
FROM message t
JOIN message m ON m.guid = CASE
    WHEN instr(t.associated_message_guid, '/') > 0
    THEN substr(t.associated_message_guid, instr(t.associated_message_guid, '/') + 1)
    ELSE t.associated_message_guid END
JOIN handle h ON m.handle_id = h.ROWID
WHERE t.is_from_me = 1
  AND t.associated_message_type BETWEEN 2000 AND 2005
  AND m.is_from_me = 0
"""

RECEIVED_SQL = """
SELECT h.id, COUNT(*)
FROM message m
JOIN handle h ON m.handle_id = h.ROWID
WHERE m.is_from_me = 0 AND m.associated_message_type = 0
GROUP BY h.id
"""


def to_ms(apple_date):
    """chat.db date column → milliseconds (epoch-agnostic, for deltas only)."""
    if abs(apple_date) > APPLE_NS_THRESHOLD:
        return apple_date / 1_000_000  # ns → ms
    return apple_date * 1000  # legacy seconds → ms


def percentile(sorted_vals, p):
    if not sorted_vals:
        return None
    k = min(len(sorted_vals) - 1, max(0, int(round(p * (len(sorted_vals) - 1)))))
    return sorted_vals[k]


def band(latencies_ms, tap_count, received_count):
    lat = sorted(latencies_ms)
    return {
        "rate": round(tap_count / received_count, 4) if received_count else 0.0,
        "n": len(lat),
        "p50_ms": int(percentile(lat, 0.50)) if lat else None,
        "p90_ms": int(percentile(lat, 0.90)) if lat else None,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--out", default=os.path.expanduser("~/.human/tapback_bands.json"))
    ap.add_argument("--min-n", type=int, default=5,
                    help="min tapback samples for a per-contact band (default 5)")
    ap.add_argument("--dry-run", action="store_true", help="print JSON, do not write")
    args = ap.parse_args()

    if not os.path.exists(args.db):
        print(f"chat.db not found: {args.db}", file=sys.stderr)
        return 1

    # Strictly read-only: URI mode=ro never takes a write lock on chat.db.
    db = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    try:
        received = dict(db.execute(RECEIVED_SQL).fetchall())

        per_contact = {}   # contact -> [latency_ms]
        all_latencies = []
        total_taps = 0
        for contact, tap_date, target_date in db.execute(LATENCY_SQL):
            if tap_date is None or target_date is None:
                continue
            total_taps += 1
            lat = to_ms(tap_date) - to_ms(target_date)
            if lat < 0:  # clock skew / bad row
                continue
            per_contact.setdefault(contact, []).append(lat)
            all_latencies.append(lat)
    finally:
        db.close()

    contacts = {}
    for contact, lats in sorted(per_contact.items()):
        if len(lats) < args.min_n:
            continue  # too few samples — this contact uses the default band
        contacts[contact] = band(lats, len(lats), received.get(contact, 0))

    doc = {
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "source": args.db,
        "default": band(all_latencies, total_taps, sum(received.values())),
        "contacts": contacts,
    }

    payload = json.dumps(doc, indent=2)
    if args.dry_run:
        print(payload)
        return 0

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    tmp = args.out + ".tmp"
    with open(tmp, "w") as f:
        f.write(payload + "\n")
    os.replace(tmp, args.out)  # atomic, same pattern as somatic_state.json
    print(f"wrote {args.out}: {len(contacts)} contact bands, "
          f"default p90={doc['default']['p90_ms']}ms n={doc['default']['n']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
