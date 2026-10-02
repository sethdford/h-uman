#!/usr/bin/env python3
"""Nightly drift self-check for the Learned Style Profile.

Compares h-uman's own outbound reply lengths per contact with Seth's learned
reply-length distribution for the same contact, and flags contacts where they
have come apart. Contract: docs/guides/learned-style.md.

  * h-uman side: memory.db `outbound_sends` (src/memory/repos/
    outbound_sends_repo_sqlite.c), channel imessage, kind text/reply, last
    --days days. Only byte LENGTHS are selected (SQL length(CAST(text AS
    BLOB))); the text column never reaches Python. Sends within 90 s of the
    previous one are one turn and their lengths are summed, the same turn rule
    the learner uses, so both sides measure the same unit.
  * Seth side: the learner's own samples (learned_style_profile.load_samples:
    same attribution, pairing, 180-day window), unweighted.

Per contact: two-sample KS statistic, median(h-uman) / median(Seth), and the
two n. A contact is FLAGGED when both sides have n >= --min-n (20) and
KS > --threshold (0.35). Flagging only reports; nothing is changed.

Output: ~/.human/logs/learned-style-drift-YYYYMMDD.json, 0600, written
atomically. Aggregates only: contacts are ordinals (c1, c2, ... by sorted
handle), never handles or names, and no text.

Exit: 0 no flags, 1 at least one flag, 2 a database could not be read
(nothing written).
"""
import argparse
import datetime as dt
import json
import os
import sqlite3
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import learned_style_profile as lsp  # noqa: E402

DEFAULT_DAYS = 30
DEFAULT_MIN_N = 20
DEFAULT_THRESHOLD = 0.35
UPTIME_STAMP_MAX_MS = 10 ** 12   # pre-fix rows stamped with uptime, not epoch


def ks_2samp(a, b):
    """Two-sample Kolmogorov-Smirnov statistic sup |F_a(x) - F_b(x)|."""
    a, b = sorted(a), sorted(b)
    na, nb = len(a), len(b)
    i = j = 0
    d = 0.0
    while i < na and j < nb:
        x = min(a[i], b[j])
        while i < na and a[i] == x:
            i += 1
        while j < nb and b[j] == x:
            j += 1
        d = max(d, abs(i / na - j / nb))
    return round(d, 4)


def huuman_turn_lengths(mem_path, since):
    """{contact: [turn byte length]} from outbound_sends. Raises
    sqlite3.Error / OSError when memory.db cannot be read."""
    if not os.path.isfile(mem_path):
        raise OSError("cannot read memory.db")
    con = sqlite3.connect(f"file:{mem_path}?mode=ro", uri=True)
    try:
        rows = con.execute(
            "select contact, sent_at_ms, length(cast(text as blob)) from outbound_sends "
            "where channel = 'imessage' and kind in ('text', 'reply') and text is not null "
            "and length(text) > 0 and sent_at_ms >= ? and sent_at_ms >= ? "
            "order by contact, sent_at_ms",
            (int(since.timestamp() * 1000), UPTIME_STAMP_MAX_MS)).fetchall()
    finally:
        con.close()
    out, last = {}, {}
    for contact, ms, n in rows:
        turns = out.setdefault(contact, [])
        if contact in last and ms - last[contact] <= lsp.BUBBLE_GAP_S * 1000:
            turns[-1] += n
        else:
            turns.append(n)
        last[contact] = ms
    return out


def build_report(seth, huuman, min_n=DEFAULT_MIN_N, threshold=DEFAULT_THRESHOLD):
    """(report, any_flagged). seth / huuman: {contact: [lengths]}. Only
    contacts with data on both sides are compared."""
    entries = []
    both = sorted(c for c in seth if seth[c] and huuman.get(c))
    for k, c in enumerate(both, 1):
        s, h = seth[c], huuman[c]
        eligible = len(s) >= min_n and len(h) >= min_n
        ks = ks_2samp(h, s)
        med_s = statistics.median(s)
        entries.append({
            "ordinal": f"c{k}",
            "n_seth": len(s),
            "n_huuman": len(h),
            "ks": ks,
            "median_seth": med_s,
            "median_huuman": statistics.median(h),
            "median_ratio": round(statistics.median(h) / med_s, 4) if med_s else None,
            "eligible": eligible,
            "flagged": eligible and ks > threshold,
        })
    flagged_n = sum(e["flagged"] for e in entries)
    report = {
        "schema": "learned-style-drift/v1",
        "min_n": min_n,
        "threshold": threshold,
        "compared_n": len(entries),
        "eligible_n": sum(e["eligible"] for e in entries),
        "flagged_n": flagged_n,
        "contacts": entries,
    }
    return report, flagged_n > 0


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--persona", default="seth")
    ap.add_argument("--persona-dir", default=None)
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.join(lsp._state_dir(), "memory.db"))
    ap.add_argument("--log-dir", default=os.path.join(lsp._state_dir(), "logs"))
    ap.add_argument("--days", type=int, default=DEFAULT_DAYS, help="h-uman window")
    ap.add_argument("--min-n", type=int, default=DEFAULT_MIN_N)
    ap.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    ap.add_argument("--now", default=None, help="ISO-8601 UTC override (tests)")
    a = ap.parse_args(argv)
    if not lsp.PERSONA_RE.match(a.persona):
        ap.error("--persona must be letters, digits, '-' or '_'")
    a.persona_dir = a.persona_dir or lsp._default_persona_dir()
    return a


def main(argv=None):
    a = parse_args(argv)
    now = lsp._now(a)
    try:
        with open(os.path.join(a.persona_dir, f"{a.persona}.json")) as f:
            contacts = lsp.learnable_contacts(json.load(f).get("contacts") or {})
        samples, _ = lsp.load_samples(a.chat_db, a.memory_db, contacts, now, dt.timezone.utc)
        huuman = huuman_turn_lengths(a.memory_db, now - dt.timedelta(days=a.days))
    except (OSError, ValueError, sqlite3.Error):
        sys.stderr.write("learned_style_drift: persona, chat.db or memory.db unreadable; "
                         "nothing written\n")
        return 2
    seth = {c: [s["len"] for s in ss] for c, ss in samples.items()}
    huuman = {c: v for c, v in huuman.items() if c in seth}
    report, flagged = build_report(seth, huuman, a.min_n, a.threshold)
    report.update(persona=a.persona, generated_at=now.strftime("%Y-%m-%dT%H:%M:%SZ"),
                  huuman_window_days=a.days, seth_window_days=lsp.WINDOW_DAYS)
    path = os.path.join(a.log_dir, f"learned-style-drift-{now:%Y%m%d}.json")
    lsp.write_atomic(path, report)
    print(f"learned_style_drift: compared {report['compared_n']}, eligible "
          f"{report['eligible_n']}, flagged {report['flagged_n']}")
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
