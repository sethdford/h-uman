#!/usr/bin/env python3
"""Flag persona contact profiles that no longer match how Seth actually texts
that person. Read-only: it never edits the persona, it reports.

Why (2026-09-26, the Lexi incident): her profile was written from the first
53 messages of a thread that reached 1,202 in 25 days. It said "friend,
moderate warmth, prefers short texts, no emoji, don't overdo it" about a
relationship that was affectionate, emoji-heavy and romantic. h-uman then sent
13 one-word replies in a row and she asked "U mad at me?". Nothing compared the
profile with the thread, so it stayed wrong for weeks.

Checks, per persona contact (only when Seth has >= --min-n of his OWN texts to
them in the window; attribution comes from eval_conversation_quality.attribute,
so h-uman's sends never count as Seth's):

  sample_outgrown    the profile's total_messages is a small fraction of the
                     real 1:1 thread (<= 1/3 and >= 100 messages behind)
  emoji_mismatch     uses_emoji says no, but >= 5% of Seth's texts to them
                     carry emoji (or says yes and n >= 50 carry none)
  short_rule_tight   prefers_short_texts caps replies at 72 chars, but
                     Seth's own p90 to them is longer
  reply_floor        reply_chars_p90 is missing, or >30% off the measured p90
  huuman_terse       h-uman's one-word share to them over the LAST 7 DAYS
                     (n >= 5 sends) is at least 40% and at least twice Seth's.
                     Recent, not the whole window: on 2026-09-26 Lexi's 60-day
                     share was 37% while the last week was 47% (11 of 15 in
                     the final two days) -- the average hid the slide.

A contact without enough data is listed as unmeasured, never as fine.
Output names contacts and numbers only, never message text.

Exit: 0 nothing stale, 1 stale profile(s) found, 2 could not measure.
"""
import argparse
import datetime as dt
import json
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eval_conversation_quality as cq  # noqa: E402
from measure_contact_reply_lengths import FIELD as REPLY_FIELD, percentile  # noqa: E402

HUUMAN_RECENT_DAYS = 7
BRIEF_CAP_SHORT = 72  # hu_conversation_brief_char_cap() with prefers_short_texts
EMOJI = re.compile("[\U0001F300-\U0001FAFF☀-➿]")
ALL_TIME = dt.datetime(2001, 1, 1, tzinfo=dt.timezone.utc)


def _one_word(text):
    return len(text.split()) <= 1


def check_contact(profile, seth_texts, huuman_texts, thread_total, min_n):
    """Findings for one contact: list of {"check", "detail"} dicts, or None
    when Seth's own texts to them are too few to measure (n < min_n)."""
    n = len(seth_texts)
    if n < min_n:
        return None
    findings = []
    recorded = profile.get("total_messages")
    if isinstance(recorded, (int, float)) and recorded > 0:
        if thread_total >= 3 * recorded and thread_total - recorded >= 100:
            findings.append({"check": "sample_outgrown",
                             "detail": f"profile built from {int(recorded)} messages; "
                                       f"the thread now has {thread_total}"})
    emoji_share = sum(1 for t in seth_texts if EMOJI.search(t)) / n
    uses = profile.get("uses_emoji")
    if uses is False and emoji_share >= 0.05:
        findings.append({"check": "emoji_mismatch",
                         "detail": f"uses_emoji=false, but {emoji_share:.0%} of Seth's "
                                   f"texts to them have emoji"})
    elif uses is True and n >= 50 and emoji_share == 0:
        findings.append({"check": "emoji_mismatch",
                         "detail": f"uses_emoji=true, but none of Seth's {n} texts "
                                   f"to them have emoji"})
    lens = [len(t.encode("utf-8")) for t in seth_texts]
    p90 = percentile(lens, 90)
    if profile.get("prefers_short_texts") is True and p90 > BRIEF_CAP_SHORT:
        findings.append({"check": "short_rule_tight",
                         "detail": f"prefers_short_texts caps replies at {BRIEF_CAP_SHORT}; "
                                   f"Seth's p90 to them is {p90}"})
    stored = profile.get(REPLY_FIELD)
    if not stored:
        findings.append({"check": "reply_floor",
                         "detail": f"{REPLY_FIELD} missing; measured {p90} "
                                   f"(run measure_contact_reply_lengths.py --write)"})
    elif abs(stored - p90) > 0.3 * p90:
        findings.append({"check": "reply_floor",
                         "detail": f"{REPLY_FIELD}={stored}, measured {p90}"})
    if len(huuman_texts) >= 5:
        hu_share = sum(1 for t in huuman_texts if _one_word(t)) / len(huuman_texts)
        seth_share = sum(1 for t in seth_texts if _one_word(t)) / n
        if hu_share >= 0.40 and hu_share >= 2 * seth_share:
            findings.append({"check": "huuman_terse",
                             "detail": f"h-uman one-word replies {hu_share:.0%} "
                                       f"(n={len(huuman_texts)}) vs Seth {seth_share:.0%}"})
    return findings


def thread_totals(chat_path):
    """All-time 1:1 message count per contact (both directions, no reactions)."""
    msgs = cq._load_messages(chat_path, ALL_TIME)
    return {c: sum(1 for m in ms if m["atype"] not in cq.REACTION_RANGE)
            for c, ms in msgs.items()}


def run(persona, chat_path, mem_path, since, min_n, now=None):
    contacts = persona.get("contacts", {})
    labeled = cq.attribute(chat_path, mem_path, since)["labeled"]
    totals = thread_totals(chat_path)
    recent = (now or dt.datetime.now(dt.timezone.utc)) - dt.timedelta(days=HUUMAN_RECENT_DAYS)
    report = {"stale": [], "unmeasured": [], "ok": []}
    for key, profile in contacts.items():
        name = profile.get("name") or "?"
        rows = labeled.get(key, [])
        seth = [m["text"] for m, label in rows if label == "seth" and m["text"]]
        huuman = [m["text"] for m, label in rows
                  if label == "huuman" and m["text"] and m["t"] >= recent]
        findings = check_contact(profile, seth, huuman, totals.get(key, 0), min_n)
        if findings is None:
            report["unmeasured"].append({"name": name, "n_seth": len(seth)})
        elif findings:
            report["stale"].append({"name": name, "n_seth": len(seth),
                                    "n_huuman_7d": len(huuman), "findings": findings})
        else:
            report["ok"].append({"name": name, "n_seth": len(seth)})
    return report


def notify(names):
    """One macOS banner naming the stale profiles (names only)."""
    if sys.platform != "darwin" or not names:
        return
    import subprocess
    body = ("Contact profiles look stale: " + ", ".join(names[:4]) +
            (" and more" if len(names) > 4 else "") +
            ". Details: ~/.human/logs/profile-check-*.json")
    subprocess.run(["/usr/bin/osascript", "-e", "on run argv", "-e",
                    'display notification (item 1 of argv) with title "h-uman"',
                    "-e", "end run", body], check=False, timeout=15)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--persona", default=os.path.expanduser("~/.human/personas/seth.json"))
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--days", type=int, default=60)
    ap.add_argument("--min-n", type=int, default=20)
    ap.add_argument("--out", help="write the JSON report here")
    ap.add_argument("--notify", action="store_true", help="macOS banner when stale")
    a = ap.parse_args(argv)

    try:
        with open(a.persona) as f:
            persona = json.load(f)
        since = dt.datetime.now(dt.timezone.utc) - dt.timedelta(days=a.days)
        report = run(persona, a.chat_db, a.memory_db, since, a.min_n)
    except (OSError, ValueError, sqlite3.Error) as e:
        print(f"REFUSE — could not measure: {e.__class__.__name__}: {e} (nothing written)")
        return 2

    for c in report["stale"]:
        print(f"STALE  {c['name']} (Seth n={c['n_seth']}, h-uman 7d n={c['n_huuman_7d']})")
        for f in c["findings"]:
            print(f"         {f['check']}: {f['detail']}")
    for c in report["ok"]:
        print(f"ok     {c['name']} (n={c['n_seth']})")
    print(f"unmeasured (<{a.min_n} of Seth's texts in {a.days}d): "
          + ", ".join(c["name"] for c in report["unmeasured"]))

    if a.out:
        report.update({"generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
                       "window_days": a.days, "min_n": a.min_n})
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        tmp = a.out + ".tmp"
        with open(tmp, "w") as f:
            json.dump(report, f, indent=2)
        os.replace(tmp, a.out)
    if report["stale"] and a.notify:
        notify([c["name"] for c in report["stale"]])
    return 1 if report["stale"] else 0


if __name__ == "__main__":
    sys.exit(main())
