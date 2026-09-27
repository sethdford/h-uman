#!/usr/bin/env python3
"""Measure how long Seth's OWN texts are, per persona contact, and store it on
the contact profile as reply_chars_p90.

Why (2026-09-26, the Lexi incident): the daemon capped 1:1 replies at the
contact's message length times ~2-2.7 with a 15-char floor, and the prompt
then said "RESPONSE LIMIT: Maximum 15 characters. Keep it tight." A contact
who texts "Heyo" got one-word replies, although Seth's median reply to her is
20 chars. Seth's reply length does not scale with theirs, so the floor comes
from what Seth actually sends that person.

Attribution reuses eval_conversation_quality.attribute(): only sends labeled
"seth" count; h-uman and ambiguous sends are excluded so the number can never
learn from h-uman's own output. Lengths are UTF-8 bytes, the unit the C cap
compares against.

Refuses to write a number it did not measure: a contact with fewer than
--min-n Seth-authored texts in the window gets no value (an existing value is
left untouched and reported as stale). Dry run by default; --write backs the
persona file up first. Prints per-contact counts and lengths only, never text.
"""
import argparse
import datetime as dt
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eval_conversation_quality as cq  # noqa: E402

FIELD = "reply_chars_p90"


def percentile(values, q):
    """Nearest-rank percentile; values non-empty."""
    s = sorted(values)
    k = max(0, min(len(s) - 1, int(round(q / 100 * len(s))) - 1))
    return s[k]


def measure(chat_path, mem_path, since, contacts, min_n):
    """{contact: {"n": int, "p50": int|None, "p90": int|None}} for each
    persona contact key; p50/p90 None when n < min_n."""
    labeled = cq.attribute(chat_path, mem_path, since)["labeled"]
    out = {}
    for contact in contacts:
        lens = [len(m["text"].encode("utf-8")) for m, label in labeled.get(contact, [])
                if label == "seth" and m["text"]]
        ok = len(lens) >= min_n
        out[contact] = {"n": len(lens),
                        "p50": percentile(lens, 50) if ok else None,
                        "p90": percentile(lens, 90) if ok else None}
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--persona", default=os.path.expanduser("~/.human/personas/seth.json"))
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--days", type=int, default=60)
    ap.add_argument("--min-n", type=int, default=20)
    ap.add_argument("--write", action="store_true", help="update the persona file (backed up)")
    a = ap.parse_args(argv)

    with open(a.persona) as f:
        persona = json.load(f)
    contacts = persona.get("contacts", {})
    since = dt.datetime.now(dt.timezone.utc) - dt.timedelta(days=a.days)
    results = measure(a.chat_db, a.memory_db, since, list(contacts), a.min_n)

    changed = 0
    for key, r in sorted(results.items(), key=lambda kv: -kv[1]["n"]):
        name = contacts[key].get("name", "?")
        old = contacts[key].get(FIELD)
        if r["p90"] is None:
            note = f"n<{a.min_n}, not measured" + (f" (existing {old} left as is)" if old else "")
            print(f"{name:24} n={r['n']:4}  {note}")
            continue
        print(f"{name:24} n={r['n']:4}  p50={r['p50']:4}  p90={r['p90']:4}  (was {old})")
        if old != r["p90"]:
            contacts[key][FIELD] = r["p90"]
            changed += 1

    if not a.write:
        print(f"dry run: {changed} contact(s) would change; pass --write to apply")
        return 0
    if changed == 0:
        print("nothing to write")
        return 0
    backup = f"{a.persona}.bak-replylen-{dt.datetime.now():%Y%m%d-%H%M%S}"
    shutil.copy2(a.persona, backup)
    tmp = a.persona + ".tmp"
    with open(tmp, "w") as f:
        json.dump(persona, f, indent=2, ensure_ascii=False)
    os.replace(tmp, a.persona)
    print(f"wrote {changed} contact(s); backup {backup}; restart the daemon to load")
    return 0


if __name__ == "__main__":
    sys.exit(main())
