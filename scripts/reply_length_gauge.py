#!/usr/bin/env python3
"""Nightly reply-length gauge (YapBench-style, two-sided): how the daemon's
sent 1:1 replies compare in length with Seth's own replies to the SAME
contact.

Why (2026-09-22 specificity measurement): 74% of the apparent specificity gap
was reply LENGTH -- the daemon's replies averaged 37.7 chars vs Seth's 71.3.
That was a one-off audit. YapBench (arXiv 2601.00624) argues for a standing,
tokenizer-free length metric against a reference distribution, in BOTH
directions: too brief is the measured problem, too long is YapBench's.

Attribution reuses eval_conversation_quality.attribute() exactly as
measure_contact_reply_lengths.py does: only messages labeled "seth" or
"huuman" count; "ambiguous" is excluded so the gauge can never blame the
daemon for a send it didn't make (or credit it for one it did). Lengths are
characters of the text (Python len(str)); bytes_p50 (UTF-8) is reported
alongside for continuity with the C reply-length cap, which compares bytes.

Per contact with >= --min-n (default 10) of BOTH seth and huuman replies in
the window: seth p10/p50/p90, huuman p10/p50/p90, median_ratio (huuman p50 /
seth p50), brevity_rate (share of huuman replies shorter than the contact's
own seth p10), excess_rate (share longer than the contact's own seth p90),
and the YapScore-style excess_chars_mean / deficit_chars_mean (mean overshoot
/ undershoot past those same two thresholds). Overall pools the same metrics
across every measured contact, weighting each REPLY equally (not each
contact), plus contacts_measured and contacts_skipped_min_n.

Refuses (exit 2, writes nothing) when chat.db or memory.db is unreadable, or
when zero contacts meet --min-n on both sides
(.claude/rules/no-number-without-a-measurement.md). Output is counts/metrics
ONLY -- no message text, no handles, no contact names. Per-contact entries
are keyed by an index (c1, c2, ...) sorted by total reply volume, never by
handle. Written 0600, atomically, to
~/.human/logs/reply-length-YYYYMMDD.json.
"""
import argparse
import datetime as dt
import json
import math
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eval_conversation_quality as cq  # noqa: E402

DEFAULT_DAYS = 14
DEFAULT_MIN_N = 10


def percentile(values, q):
    """Nearest-rank percentile; values non-empty. Same rule as
    measure_contact_reply_lengths.percentile, so the two tools never
    disagree about what "p50" means for the same data."""
    s = sorted(values)
    k = max(0, min(len(s) - 1, int(round(q / 100 * len(s))) - 1))
    return s[k]


def _stats(lens, byte_lens):
    """{"n", "p10", "p50", "p90", "bytes_p50"} over a non-empty list of char
    lengths, plus the UTF-8 byte p50 (the unit the C reply-length cap
    actually compares against)."""
    return {
        "n": len(lens),
        "p10": percentile(lens, 10),
        "p50": percentile(lens, 50),
        "p90": percentile(lens, 90),
        "bytes_p50": percentile(byte_lens, 50),
    }


def _compare(seth_stats, huuman_lens):
    """YapScore-style two-sided comparison of huuman_lens against seth's own
    p10/p50/p90 thresholds. huuman_lens must be non-empty."""
    p10, p50, p90 = seth_stats["p10"], seth_stats["p50"], seth_stats["p90"]
    n = len(huuman_lens)
    brevity = sum(1 for x in huuman_lens if x < p10) / n
    excess_rate = sum(1 for x in huuman_lens if x > p90) / n
    excess_chars = sum(max(0, x - p90) for x in huuman_lens) / n
    deficit_chars = sum(max(0, p10 - x) for x in huuman_lens) / n
    huuman_p50 = percentile(huuman_lens, 50)
    return {
        "median_ratio": (huuman_p50 / p50) if p50 else None,
        "brevity_rate": brevity,
        "excess_rate": excess_rate,
        "excess_chars_mean": excess_chars,
        "deficit_chars_mean": deficit_chars,
    }


def ks_2samp(a, b):
    """Two-sample Kolmogorov-Smirnov over non-empty lists: (D, asymptotic
    two-sided p). stdlib-only so the gauge keeps no scipy dependency; D is
    exact, p uses the Stephens small-sample correction."""
    a, b = sorted(a), sorted(b)
    n, m = len(a), len(b)
    i = j = 0
    d = 0.0
    while i < n and j < m:
        x = min(a[i], b[j])
        while i < n and a[i] <= x:
            i += 1
        while j < m and b[j] <= x:
            j += 1
        d = max(d, abs(i / n - j / m))
    en = math.sqrt(n * m / (n + m))
    lam = (en + 0.12 + 0.11 / en) * d
    if lam < 1e-9:
        return d, 1.0
    p = 2.0 * sum((-1) ** (k - 1) * math.exp(-2.0 * k * k * lam * lam) for k in range(1, 101))
    return d, max(0.0, min(1.0, p))


def _metrics(seth_lens, seth_blens, huuman_lens, huuman_blens):
    """Full metric bundle for one seth/huuman pair of (non-empty) length
    lists -- used for both a single contact and the pooled overall."""
    seth = _stats(seth_lens, seth_blens)
    huuman = _stats(huuman_lens, huuman_blens)
    out = {"n_seth": seth["n"], "n_huuman": huuman["n"], "seth": seth, "huuman": huuman}
    out.update(_compare(seth, huuman_lens))
    # Byte lengths: the unit the HU_LENGTH_POLICY cap compares against.
    out["ks_d"], out["ks_p"] = ks_2samp(huuman_blens, seth_blens)
    return out


def measure(chat_path, mem_path, since, min_n):
    """-> {"contacts": {"c1": {...}, ...}, "overall": {...}} or None on
    refusal: chat.db/memory.db unreadable, or no contact has >= min_n of
    BOTH seth- and huuman-labeled replies in the window. Never touches
    ~/.human or chat.db for anything but a read-only connection (via
    eval_conversation_quality.attribute); writes nothing itself."""
    try:
        labeled = cq.attribute(chat_path, mem_path, since)["labeled"]
    except sqlite3.Error:
        return None

    per_contact = []
    pooled_seth, pooled_seth_b = [], []
    pooled_huuman, pooled_huuman_b = [], []
    skipped = 0
    for _contact, rows in labeled.items():
        seth_lens, seth_b, huuman_lens, huuman_b = [], [], [], []
        for m, label in rows:
            text = m["text"]
            if not text or label == "ambiguous":
                continue
            n_chars, n_bytes = len(text), len(text.encode("utf-8"))
            if label == "seth":
                seth_lens.append(n_chars)
                seth_b.append(n_bytes)
            elif label == "huuman":
                huuman_lens.append(n_chars)
                huuman_b.append(n_bytes)
        if len(seth_lens) < min_n or len(huuman_lens) < min_n:
            if seth_lens or huuman_lens:
                skipped += 1
            continue
        metrics = _metrics(seth_lens, seth_b, huuman_lens, huuman_b)
        metrics["_sort_n"] = metrics["n_seth"] + metrics["n_huuman"]
        per_contact.append(metrics)
        pooled_seth.extend(seth_lens)
        pooled_seth_b.extend(seth_b)
        pooled_huuman.extend(huuman_lens)
        pooled_huuman_b.extend(huuman_b)

    if not per_contact:
        return None

    per_contact.sort(key=lambda c: -c.pop("_sort_n"))
    contacts = {f"c{i}": c for i, c in enumerate(per_contact, 1)}

    overall = _metrics(pooled_seth, pooled_seth_b, pooled_huuman, pooled_huuman_b)
    overall["contacts_measured"] = len(per_contact)
    overall["contacts_skipped_min_n"] = skipped

    return {"contacts": contacts, "overall": overall}


def _write_json_private(path, payload):
    """0600 from creation, atomic: mkstemp (0600, O_EXCL, never follows a
    planted symlink, never inherits an existing file's wider mode) + rename
    over path. Same pattern as curator_names.write_jsonl_private."""
    d = os.path.dirname(path) or "."
    os.makedirs(d, mode=0o700, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".reply-length-", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(payload, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--days", type=int, default=DEFAULT_DAYS)
    ap.add_argument("--min-n", type=int, default=DEFAULT_MIN_N)
    ap.add_argument("--out-dir", default=os.path.expanduser("~/.human/logs"))
    ap.add_argument("--now", help=argparse.SUPPRESS)  # test hook: ISO-8601 instant
    a = ap.parse_args(argv)

    now = (dt.datetime.fromisoformat(a.now) if a.now else dt.datetime.now(dt.timezone.utc))
    since = now - dt.timedelta(days=a.days)

    result = measure(a.chat_db, a.memory_db, since, a.min_n)
    if result is None:
        print("refused: chat.db/memory.db unreadable, or no contact has "
              f">= --min-n ({a.min_n}) of both seth and huuman replies -- "
              "nothing written", file=sys.stderr)
        return 2

    ov = result["overall"]
    print(f"reply-length gauge: median_ratio={ov['median_ratio']:.3f}  "
          f"brevity_rate={ov['brevity_rate']:.3f}  excess_rate={ov['excess_rate']:.3f}  "
          f"ks_d={ov['ks_d']:.3f} ks_p={ov['ks_p']:.4f}  "
          f"contacts_measured={ov['contacts_measured']}")

    payload = {
        "generated_at": now.isoformat(),
        "since": since.isoformat(),
        "days": a.days,
        "min_n": a.min_n,
        "unit": "chars (Python len); bytes_p50 fields are UTF-8 bytes",
        "contacts": result["contacts"],
        "overall": ov,
    }
    out_path = os.path.join(a.out_dir, f"reply-length-{now.strftime('%Y%m%d')}.json")
    _write_json_private(out_path, payload)
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
