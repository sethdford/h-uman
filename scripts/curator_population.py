"""Who the sleep-time curator reads each night (spec §3).

Pure functions over eval_conversation_quality.attribute() timelines, so the
selection is testable without chat.db or a model. The wide pass skips persona
contacts: the existing persona pass already curates them from memory.db.
"""
import contextlib
import datetime as dt
import json
import os
import re
import sqlite3

WINDOW_DAYS = 30
MIN_THEM = 10
MIN_ME = 5
NEVER_PATH = os.path.expanduser("~/.human/curator_never.json")
E164_RE = re.compile(r"\+\d{7,15}")
EMAIL_RE = re.compile(r"[^@\s]+@[^@\s]+\.[^@\s]+")


def is_short_code(handle):
    return bool(re.fullmatch(r"\d{3,6}", (handle or "").strip()))


def is_valid_handle(handle):
    """Spec §3: +E.164 or an email -- not a short code, a bare local number,
    or a business/urn handle."""
    return bool(E164_RE.fullmatch(handle or "") or EMAIL_RE.fullmatch(handle or ""))


def normalize_handle(handle):
    """Canonical form for opt-out matching: emails lowercased; phones as
    "+" + digits, with a bare 10-digit number read as US (+1). Anything
    else is returned stripped. The stored contact_id is never rewritten."""
    s = (handle or "").strip()
    if "@" in s:
        return s.lower()
    if not re.fullmatch(r"\+?[\d\s().-]+", s):
        return s
    digits = re.sub(r"\D", "", s)
    if len(digits) == 10 and not s.startswith("+"):
        digits = "1" + digits
    return "+" + digits if digits else s


def normalize_all(handles):
    return {normalize_handle(h) for h in handles or ()}


def eligible_handles(timelines, persona_ids, now, window_days=WINDOW_DAYS,
                     min_them=MIN_THEM, min_me=MIN_ME, exclude=()):
    """exclude: extra handles never curated (the daemon's loopback handle),
    matched after normalization."""
    cutoff = now - dt.timedelta(days=window_days)
    excluded = normalize_all(exclude)
    out = []
    for handle, msgs in timelines.items():
        if not is_valid_handle(handle) or handle in persona_ids:
            continue
        if normalize_handle(handle) in excluded:
            continue
        recent = [m for m in msgs if m["t"] >= cutoff]
        them = sum(1 for m in recent if not m["from_me"])
        me = sum(1 for m in recent if m["from_me"])
        if them >= min_them and me >= min_me:
            out.append(handle)
    return sorted(out)


def load_suppressed(mem_db_path):
    """Load opted-out contacts from memory.db's contact_suppressions table.

    Opt-outs are a hard privacy exclusion, so this fails closed: only the
    narrow "table not created yet" case is treated as "nobody has opted
    out". Every other sqlite3.Error (unreadable/locked/corrupt DB, a wrong
    path, permission denied, etc.) is RE-RAISED so the caller refuses the
    run rather than silently reading contacts who opted out.
    """
    with contextlib.closing(sqlite3.connect(f"file:{mem_db_path}?mode=ro", uri=True)) as con:
        try:
            rows = con.execute("SELECT contact FROM contact_suppressions").fetchall()
        except sqlite3.OperationalError as e:
            if "no such table" in str(e):
                return set()  # table not created yet = nobody has opted out
            raise
    return {r[0] for r in rows if r[0]}


def load_never(path=NEVER_PATH):
    if not os.path.exists(path):
        return set()
    data = json.load(open(path))
    if not isinstance(data, list) or not all(isinstance(x, str) for x in data):
        raise ValueError(f"{path}: expected a JSON list of handles")
    return set(data)


def exclusion_reason(handle, suppressed, never):
    """Exact match OR normalized match: a hand-written never-file entry
    ("5550000042", "Friend@Example.com") still excludes the chat.db handle."""
    n = normalize_handle(handle)
    if handle in suppressed or n in normalize_all(suppressed):
        return "suppressed"
    if handle in never or n in normalize_all(never):
        return "never"
    return None


def unmatched_never(never, handles):
    """How many never-file entries match no chat.db handle (a typo'd opt-out
    silently protects nobody). Count only: callers must not print entries."""
    known = normalize_all(handles)
    return sum(1 for h in normalize_all(never) if h not in known)


def load_loopback_handles(config_path):
    """channels.imessage.loopback_handle (a string or a list) from the
    daemon's config.json, normalized. Missing file or key -> empty set; a
    malformed file raises ValueError so the caller refuses."""
    if not os.path.exists(config_path):
        return set()
    with open(config_path) as f:
        cfg = json.load(f)
    imsg = ((cfg.get("channels") or {}).get("imessage") or {}) if isinstance(cfg, dict) else {}
    lb = imsg.get("loopback_handle") if isinstance(imsg, dict) else None
    if isinstance(lb, str):
        lb = [lb]
    if not isinstance(lb, list):
        return set()
    return normalize_all(h for h in lb if isinstance(h, str) and h.strip())
