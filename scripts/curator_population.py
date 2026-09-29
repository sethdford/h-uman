"""Who the sleep-time curator reads each night (spec §3).

Pure functions over eval_conversation_quality.attribute() timelines, so the
selection is testable without chat.db or a model. The wide pass skips persona
contacts: the existing persona pass already curates them from memory.db.
"""
import datetime as dt
import json
import os
import re
import sqlite3

WINDOW_DAYS = 30
MIN_THEM = 10
MIN_ME = 5
NEVER_PATH = os.path.expanduser("~/.human/curator_never.json")


def is_short_code(handle):
    return bool(re.fullmatch(r"\d{3,6}", (handle or "").strip()))


def eligible_handles(timelines, persona_ids, now, window_days=WINDOW_DAYS,
                     min_them=MIN_THEM, min_me=MIN_ME):
    cutoff = now - dt.timedelta(days=window_days)
    out = []
    for handle, msgs in timelines.items():
        if not handle or is_short_code(handle) or handle in persona_ids:
            continue
        recent = [m for m in msgs if m["t"] >= cutoff]
        them = sum(1 for m in recent if not m["from_me"])
        me = sum(1 for m in recent if m["from_me"])
        if them >= min_them and me >= min_me:
            out.append(handle)
    return sorted(out)


def load_suppressed(mem_db_path):
    try:
        con = sqlite3.connect(f"file:{mem_db_path}?mode=ro", uri=True)
        rows = con.execute("SELECT contact FROM contact_suppressions").fetchall()
        con.close()
    except sqlite3.Error:
        return set()  # table not created yet = nobody has opted out
    return {r[0] for r in rows if r[0]}


def load_never(path=NEVER_PATH):
    if not os.path.exists(path):
        return set()
    data = json.load(open(path))
    if not isinstance(data, list) or not all(isinstance(x, str) for x in data):
        raise ValueError(f"{path}: expected a JSON list of handles")
    return set(data)


def exclusion_reason(handle, suppressed, never):
    if handle in suppressed:
        return "suppressed"
    if handle in never:
        return "never"
    return None
