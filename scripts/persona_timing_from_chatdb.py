#!/usr/bin/env python3
"""Fill the persona's timing sections from Seth's own texting history.

seth.json validated on 2026-09-20 with every timing section absent:
`timezone`, `chronotype`, `time_overlays`, `daily_routine`. Those feed
better-than-human criterion 4 (timing) and the reply-delay/bookend policies.
Rather than invent them, derive them from what chat.db already records:
the hour of day of every text Seth himself sent.

Reads ~/Library/Messages/chat.db READ-ONLY (uri mode=ro; never copied).
Counts only `is_from_me=1` rows; uses the row's `date` (Apple ns) rendered
in local time by sqlite. Does not read message text.

Derivations (all thresholds are printed into the JSON so they can be audited):
  chronotype       morning_lark if >= 25% of sends fall before 09:00,
                   evening_owl if >= 25% fall at/after 21:00, else
                   intermediate; unknown below --min-n sends.
  time_overlays    one sentence per slot quoting the measured share.
  daily_routine    contiguous hour blocks per weekday/weekend, availability
                   high/medium/low by share of that day-type's sends
                   (>= 1.5x the uniform share / >= 0.5x / below).

Default is dry-run (prints the fragment). --write backs the persona up to
seth.json.bak-timing-<ts> and rewrites only these four keys; every other key
(voice, voice_messages, core, ...) is preserved byte-for-byte in content.

Usage: scripts/persona_timing_from_chatdb.py [--days 180] [--write]
"""
import argparse
import json
import os
import sqlite3
import sys
import time
from datetime import datetime

APPLE_EPOCH = 978307200
LARK_SHARE, OWL_SHARE = 0.25, 0.25
HIGH_X, LOW_X = 1.5, 0.5


def open_ro(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


def histogram(chat_db, days, now=None):
    """{'weekday': [24 counts], 'weekend': [24 counts]} of Seth's outbound texts."""
    now = now or int(time.time())
    cutoff = now - days * 86400
    con = open_ro(chat_db)
    try:
        rows = con.execute(
            "SELECT strftime('%w', datetime(date/1000000000 + ?, 'unixepoch', 'localtime')), "
            "       strftime('%H', datetime(date/1000000000 + ?, 'unixepoch', 'localtime')), COUNT(*) "
            "FROM message WHERE is_from_me=1 AND date/1000000000 + ? > ? GROUP BY 1, 2",
            (APPLE_EPOCH, APPLE_EPOCH, APPLE_EPOCH, cutoff)).fetchall()
    finally:
        con.close()
    h = {"weekday": [0] * 24, "weekend": [0] * 24}
    for dow, hr, n in rows:
        h["weekend" if dow in ("0", "6") else "weekday"][int(hr)] += n
    return h


def classify_chronotype(h, min_n):
    total = sum(h["weekday"]) + sum(h["weekend"])
    if total < min_n:
        return "unknown", {"n": total, "before_09": None, "after_21": None}
    early = sum(h["weekday"][:9]) + sum(h["weekend"][:9])
    late = sum(h["weekday"][21:]) + sum(h["weekend"][21:])
    shares = {"n": total, "before_09": round(early / total, 3), "after_21": round(late / total, 3)}
    if shares["before_09"] >= LARK_SHARE:
        return "morning_lark", shares
    if shares["after_21"] >= OWL_SHARE:
        return "evening_owl", shares
    return "intermediate", shares


def _share(h, lo, hi):
    tot = sum(h["weekday"]) + sum(h["weekend"])
    part = sum(h["weekday"][lo:hi]) + sum(h["weekend"][lo:hi])
    return part / tot if tot else 0.0


def time_overlays(h):
    def pct(x):
        return f"{100 * x:.0f}%"
    return {
        "late_night": f"Only {pct(_share(h, 22, 24) + _share(h, 0, 5))} of your texts go out between 10pm and 5am. "
                      "If you are replying this late, keep it to one short line.",
        "early_morning": f"{pct(_share(h, 5, 9))} of your texts are sent before 9am. "
                         "Early replies are brief and practical.",
        "afternoon": f"{pct(_share(h, 12, 18))} of your texts are sent between noon and 6pm — your busiest window. "
                     "Normal length and pace.",
        "evening": f"{pct(_share(h, 18, 22))} of your texts are sent between 6pm and 10pm. "
                   "Conversational, unhurried.",
    }


def routine_blocks(counts):
    """Collapse 24 hourly counts into contiguous blocks with an availability label."""
    total = sum(counts)
    if not total:
        return []
    uniform = total / 24
    label = lambda n: "high" if n >= HIGH_X * uniform else ("medium" if n >= LOW_X * uniform else "low")
    blocks, start = [], 0
    for hr in range(1, 25):
        if hr == 24 or label(counts[hr]) != label(counts[start]):
            n = sum(counts[start:hr])
            av = label(counts[start])
            blocks.append({
                "time": f"{start:02d}-{hr % 24:02d}",
                "activity": {"high": "peak texting window", "medium": "light texting", "low": "quiet"}[av],
                "availability": av,
                "mood_modifier": "",
                "measured_share": round(n / total, 3),
            })
            start = hr
    return blocks


def local_timezone():
    try:
        link = os.readlink("/etc/localtime")
        return link.split("zoneinfo/")[-1]
    except OSError:
        return datetime.now().astimezone().tzname() or "UTC"


def build_fragment(h, min_n, tz):
    chrono, shares = classify_chronotype(h, min_n)
    return {
        "timezone": tz,
        "chronotype": chrono,
        "time_overlays": time_overlays(h),
        "daily_routine": {
            "routine_variance": 0.15,
            "weekday": routine_blocks(h["weekday"]),
            "weekend": routine_blocks(h["weekend"]),
            "measured": {"source": "chat.db is_from_me=1", "chronotype_shares": shares,
                         "thresholds": {"lark_share": LARK_SHARE, "owl_share": OWL_SHARE,
                                        "high_x_uniform": HIGH_X, "low_x_uniform": LOW_X},
                         "measured_at": datetime.now().strftime("%Y-%m-%d")},
        },
    }


def write_persona(persona_path, fragment):
    with open(persona_path, encoding="utf-8") as f:
        persona = json.load(f)
    backup = f"{persona_path}.bak-timing-{int(time.time())}"
    with open(persona_path, "rb") as src, open(backup, "wb") as dst:
        dst.write(src.read())
    persona.update(fragment)
    tmp = persona_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(persona, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, persona_path)
    return backup


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--persona", default=os.path.expanduser("~/.human/personas/seth.json"))
    ap.add_argument("--days", type=int, default=180)
    ap.add_argument("--min-n", type=int, default=100)
    ap.add_argument("--timezone", default=None)
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args(argv)

    h = histogram(args.chat_db, args.days)
    frag = build_fragment(h, args.min_n, args.timezone or local_timezone())
    print(json.dumps(frag, indent=2))
    if args.write:
        backup = write_persona(args.persona, frag)
        print(f"wrote {args.persona} (backup: {backup})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
