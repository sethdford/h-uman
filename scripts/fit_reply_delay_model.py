#!/usr/bin/env python3
"""Contract C5, Part C — fit Seth's reply-delay distribution from chat.db.

No ML library: this is a plain quantile table, hierarchically bucketed and
with hierarchical fallback for sparse cells. For every inbound (contact ->
Seth) DM message with a reply within --max-delay-hours, we record Seth's
reply delay (seconds) and bucket it by:

  hour   — hour-of-day (0-23) the inbound message arrived, Seth's local time
           is not resolved here (chat.db has no tz info); this uses the
           wall-clock hour of the UTC-normalized apple-epoch timestamp
           converted via time.localtime on THIS machine, which is the
           machine the daemon also runs on — same convention, so the
           bucket is meaningful for the C loader's `hour` parameter.
  len    — incoming message length bucket: short (<40 chars),
           medium (40-160), long (>160)
  freq   — contact reply-frequency tercile (low/mid/high), computed from
           each contact's total DM message count in the lookback window,
           split at that population's 33rd/67th percentiles

For each (hour, len, freq) cell we compute p10/p25/p50/p75/p90 when the
cell has >= --min-cell-n samples (default 5); otherwise we fall back to
the (hour, len) marginal, then the (hour) marginal, then the global
distribution — all four levels are written to the output JSON so the C
loader (src/daemon/daemon_reply_delay.c) can walk the same fallback chain
without needing chat.db access itself.

Output: ~/.human/reply_delay_model.json (path overridable via --out).
Nothing leaves the machine; the output contains only bucket keys, counts,
and delay-second quantiles — no message content, no contact identifiers.

Refuses (exit 2, writes nothing) below --min-n total samples (default 30).
"""
import argparse
import json
import os
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
from eval_when_to_speak import APPLE_EPOCH, open_ro, apple_ns_to_unix, load_dm_messages  # noqa: E402

try:
    from imessage_text import decode_attributed_body  # noqa: E402
except ImportError:
    decode_attributed_body = None

LENGTH_BUCKETS = ("short", "medium", "long")
FREQ_TERCILES = ("low", "mid", "high")
QUANTILE_LEVELS = (0.10, 0.25, 0.50, 0.75, 0.90)


def length_bucket(n_chars, thresholds):
    lo, hi = thresholds
    if n_chars < lo:
        return "short"
    if n_chars <= hi:
        return "medium"
    return "long"


def freq_tercile(freq, boundaries):
    lo, hi = boundaries
    if freq <= lo:
        return "low"
    if freq <= hi:
        return "mid"
    return "high"


def percentile(sorted_vals, p):
    """Linear-interpolation percentile, p in [0,1]. sorted_vals must be
    non-empty and sorted ascending."""
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    idx = p * (len(sorted_vals) - 1)
    lo = int(idx)
    hi = min(lo + 1, len(sorted_vals) - 1)
    frac = idx - lo
    return sorted_vals[lo] * (1 - frac) + sorted_vals[hi] * frac


def quantiles_for(delays):
    if not delays:
        return None
    s = sorted(delays)
    return {f"p{int(p * 100)}": percentile(s, p) for p in QUANTILE_LEVELS}


def load_message_lengths(chat_db, since_unix):
    """Returns {(chat_id, ts_unix): char_length} for every INBOUND message,
    keyed by the SAME (chat_id, apple_ns_to_unix(date)) transform
    load_dm_messages uses — so lookups are exact float equality, not a
    nearest-neighbor guess.

    Modern macOS stores most message bodies ONLY in `attributedBody` (a
    typedstream/NSArchiver blob), not the legacy `text` column — see
    extract_imessage_pairs.py's extract_text_from_attributed_body docstring
    for the full history of that decoder. Falling back to `LENGTH(m.text)`
    alone made length_bucket_thresholds collapse to (0, 0) in practice
    (verified against this machine's real chat.db: every message landed in
    'short'), which is exactly the kind of silently-degenerate measurement
    ~/.claude/rules/no-number-without-a-measurement.md warns about — so we
    decode attributedBody via the SAME shared decoder
    scripts/blind_ab/imessage_text.py uses, when `text` is empty. When the
    decoder isn't importable (only scripts/blind_ab layout drifted), we
    still return the text-column length rather than crashing — a degraded
    length signal, not a missing one."""
    cur = chat_db.cursor()
    since_apple_ns = (since_unix - APPLE_EPOCH) * 1_000_000_000
    cur.execute(
        """
        SELECT cmj.chat_id, m.date, m.is_from_me, m.text, m.attributedBody
        FROM message m
        JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        WHERE m.date >= ? AND m.is_system_message = 0 AND m.item_type = 0
        """,
        (since_apple_ns,),
    )
    lengths = {}
    for chat_id, date_ns, is_from_me, text, attributed_body in cur.fetchall():
        if is_from_me:
            continue
        ts = apple_ns_to_unix(date_ns)
        if ts <= 0:
            continue
        if text:
            tlen = len(text)
        elif attributed_body and decode_attributed_body is not None:
            decoded = decode_attributed_body(attributed_body)
            tlen = len(decoded) if decoded else 0
        else:
            tlen = 0
        lengths[(chat_id, ts)] = tlen
    return lengths


def build_delay_samples(chat_db, since_unix, max_delay_secs):
    """Returns (samples, contact_msg_counts). Each sample is
    {hour, len_chars, contact, delay_secs, ts} — `ts` (the inbound
    message's unix timestamp) is carried for callers that need a
    time-ordered split (e.g. scripts/eval_reply_delay_model.py's
    held-out evaluation); fit() below ignores it."""
    messages = load_dm_messages(chat_db, since_unix)
    lengths = load_message_lengths(chat_db, since_unix)

    by_chat = {}
    contact_msg_counts = {}
    for chat_id, contact, ts, is_from_me in messages:
        by_chat.setdefault(chat_id, []).append((ts, is_from_me, contact))
        contact_msg_counts[contact] = contact_msg_counts.get(contact, 0) + 1

    samples = []
    for chat_id, rows in by_chat.items():
        rows.sort(key=lambda r: r[0])
        outbound_ts = [ts for ts, ifm, _ in rows if ifm]
        for ts, is_from_me, contact in rows:
            if is_from_me:
                continue
            candidates = [o for o in outbound_ts if ts < o <= ts + max_delay_secs]
            if not candidates:
                continue
            delay = min(candidates) - ts
            tlen = lengths.get((chat_id, ts), 0)
            hour = time.localtime(ts).tm_hour
            samples.append(
                {"hour": hour, "len_chars": tlen, "contact": contact, "delay_secs": delay, "ts": ts}
            )
    return samples, contact_msg_counts


def fit(samples, contact_msg_counts, min_cell_n):
    if not samples:
        return None

    all_lens = sorted(s["len_chars"] for s in samples)
    len_lo = percentile(all_lens, 0.33)
    len_hi = percentile(all_lens, 0.67)
    length_thresholds = (len_lo, len_hi)

    all_freqs = sorted(contact_msg_counts.values())
    freq_lo = percentile(all_freqs, 0.33)
    freq_hi = percentile(all_freqs, 0.67)
    freq_boundaries = (freq_lo, freq_hi)

    for s in samples:
        s["len_bucket"] = length_bucket(s["len_chars"], length_thresholds)
        freq = contact_msg_counts.get(s["contact"], 0)
        s["freq_tercile"] = freq_tercile(freq, freq_boundaries)

    def group_delays(pred):
        return [s["delay_secs"] for s in samples if pred(s)]

    cells = {}
    for hour in range(24):
        for lb in LENGTH_BUCKETS:
            for ft in FREQ_TERCILES:
                key = f"h{hour}_l{lb}_f{ft}"
                d = group_delays(lambda s, hour=hour, lb=lb, ft=ft: s["hour"] == hour
                                  and s["len_bucket"] == lb and s["freq_tercile"] == ft)
                if len(d) >= min_cell_n:
                    cells[key] = {"n": len(d), "quantiles": quantiles_for(d)}

    hour_len_marginals = {}
    for hour in range(24):
        for lb in LENGTH_BUCKETS:
            key = f"h{hour}_l{lb}"
            d = group_delays(lambda s, hour=hour, lb=lb: s["hour"] == hour and s["len_bucket"] == lb)
            if len(d) >= min_cell_n:
                hour_len_marginals[key] = {"n": len(d), "quantiles": quantiles_for(d)}

    hour_marginals = {}
    for hour in range(24):
        key = f"h{hour}"
        d = group_delays(lambda s, hour=hour: s["hour"] == hour)
        if len(d) >= min_cell_n:
            hour_marginals[key] = {"n": len(d), "quantiles": quantiles_for(d)}

    global_delays = [s["delay_secs"] for s in samples]

    return {
        "length_bucket_thresholds": {"lo_chars": length_thresholds[0], "hi_chars": length_thresholds[1]},
        "freq_tercile_boundaries": {"lo_count": freq_boundaries[0], "hi_count": freq_boundaries[1]},
        "min_cell_n": min_cell_n,
        "cells": cells,
        "hour_len_marginals": hour_len_marginals,
        "hour_marginals": hour_marginals,
        "global": {"n": len(global_delays), "quantiles": quantiles_for(global_delays)},
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--days", type=int, default=90)
    ap.add_argument("--max-delay-hours", type=float, default=24.0,
                    help="a reply beyond this is treated as no-reply, not a delay sample")
    ap.add_argument("--min-cell-n", type=int, default=5)
    ap.add_argument("--min-n", type=int, default=30, help="refuse below this many total samples")
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--out", default=os.path.expanduser("~/.human/reply_delay_model.json"))
    args = ap.parse_args()

    now = time.time()
    since = now - args.days * 86400.0

    chat_db = open_ro(args.chat_db)
    if chat_db is None:
        print(f"REFUSE: chat.db not found at {args.chat_db}", file=sys.stderr)
        return 2

    samples, contact_msg_counts = build_delay_samples(chat_db, since, args.max_delay_hours * 3600.0)
    if len(samples) < args.min_n:
        print(f"REFUSE: only {len(samples)} delay samples (min_n={args.min_n})", file=sys.stderr)
        return 2

    model = fit(samples, contact_msg_counts, args.min_cell_n)
    model["generated_at"] = int(now)
    model["days"] = args.days
    model["max_delay_hours"] = args.max_delay_hours
    model["n_samples"] = len(samples)

    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(model, f, indent=2)

    print(f"wrote {args.out}")
    print(f"n_samples={len(samples)} cells={len(model['cells'])} "
          f"hour_marginals={len(model['hour_marginals'])}")
    print("median delay per hour bucket (fallback chain applied where a cell is empty):")
    for hour in range(24):
        key = f"h{hour}"
        m = model["hour_marginals"].get(key)
        if m and m["quantiles"]:
            print(f"  hour={hour:02d} n={m['n']:4d} median={m['quantiles']['p50']:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
