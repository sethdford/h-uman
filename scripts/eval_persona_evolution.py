#!/usr/bin/env python3
"""eval_persona_evolution.py — measure event-induced register shifts in
Seth's own outbound iMessages, as the "BFI-Adapt" persona-evolution check
for gap #9 (docs/research/2026-09-02-august-2026-sota-gap-analysis.md).

READ-ONLY. Opens chat.db with mode=ro + immutable=1 and never writes to it,
never copies rows out of it, and never prints/logs message text. Only
counts, rates, and dates leave this script (stdout, and an optional
--out JSON file gated the same way).

What this measures
-------------------
For each life event (a date Seth's persona notes something changed about
him), take his own outbound texts (is_from_me=1) in a 30-day window before
the event and a 30-day window after it, and compute nine per-message style
axes (length, lowercase-start, terminal punctuation, question rate, emoji
rate, exclamation rate, two formality-proxy components, one warmth-proxy
component). Report the before/after means with bootstrap 95% CIs so a
downstream gate can ask "did the axis move by more than noise, and if so,
did the persona/adapter output move the same direction?" (the second half
is NOT computed by this script — see docs/plans/2026-09-02-persona-evolution/
spec.md for the gate definition and how it's wired to a matched-prompt
generation pass.)

Event dates
-----------
Sourced from ~/.human/personas/seth.json and its dated backups (see EVENTS
below for exact file:line citations). Neither date is pinned to +/-7 days:
"summer 2026" / "as of July 2026" are the only real-world anchors in the
persona text; the more precise-looking dates below are the moments the
persona's OWN fact record flipped from the pre-event to the post-event
description, which is a proxy for "the assistant found out," not "the
event occurred." This script prints that caveat every run (see
--explain-dates) and the spec.md carries the full derivation.

Refusal contract (see .claude/rules/no-number-without-a-measurement.md)
------------------------------------------------------------------------
If ANY window (pre or post, for ANY event being analyzed) has fewer than
--min-n messages (default 100), OR spans fewer than --min-covered-days
(default 20) between its first and last present message, this script:
  - prints a diagnostic to stdout tagged "status": "INSUFFICIENT_DATA"
  - exits with a non-zero status
  - writes NOTHING to --out, regardless of whether --out was given

A number this script did not measure is worse than no number.

Second store: --source (added 2026-09-03)
---------------------------------------------
chat.db on this machine keeps 30 days, so pre-event windows for the two
summer-2026 events are empty there. --source PATH (repeatable) merges a
JSONL export that carries Seth-authorship provenance from chat.db's
is_from_me=1 column through to the record. Two shapes are accepted, both
produced by scripts/extract_imessage_pairs.py:
  - training_pairs.jsonl: {"messages": [..., {"role": "assistant", ...}],
    "metadata": {"timestamp": <local ISO>}} -- the final assistant turn is
    the is_from_me=1 message (extract_imessage_pairs.py:256-267).
  - ground_truth.jsonl: {"seth_reply": ..., "timestamp": <local ISO>}
    (extract_imessage_pairs.py:297-315).
Anything else (DPO prompt/chosen/rejected, memory.db dumps) is REJECTED:
those rows are the daemon's output or a contact's text, not Seth's.
Export timestamps are LOCAL naive (datetime.fromtimestamp); chat.db rows
here are UTC naive, so export rows are converted to UTC before windowing
and before de-dup. De-dup key = (timestamp to the second, sha256 of the
stripped text); chat.db rows win, export rows are dropped on collision.
When any --source is given, chat.db rows are also passed through the
export's own sampling frame (len >= 2, MIN_REPLY_LENGTH) so both windows
are drawn with the same length floor; the export's other exclusion (a
Seth message that opens a conversation window with no prior context is
not exported) cannot be reproduced from chat.db and is reported as a
caveat, not silently ignored. Every window also reports its actual
coverage (first/last timestamp, covered_days) so a window that is
"n=123 over 5 days" is never read as a 30-day window.

Usage
-----
    python3 scripts/eval_persona_evolution.py --event both
    python3 scripts/eval_persona_evolution.py --event both \
        --source data/imessage/training_pairs.jsonl \
        --source ~/.human/logs/eval-archive/imessage-corpus-backup-20260725-113543/training_pairs.jsonl
    python3 scripts/eval_persona_evolution.py --event job --out results.json
    python3 scripts/eval_persona_evolution.py --explain-dates

    # Windowed re-derivation (AC-7.2): trailing N-day re-derivation of all 9
    # axes, reusing this script's own axis/CI/refusal-contract functions.
    # NOTE: this is the corrected nightly command -- "--full-range
    # --window-days 30" (as spec.md section 5 originally prescribed) does
    # NOT do what it says: --window-days is read only by the event pre/post
    # bucketer, never by --full-range, so that command silently reports the
    # entire --start/--end range instead of a 30-day trailing window (see
    # spec.md section 8). Use --trailing-days for an honest windowed report:
    python3 scripts/eval_persona_evolution.py --event none --trailing-days 30 \
        --source data/imessage/training_pairs.jsonl \
        --source ~/.human/logs/eval-archive/imessage-corpus-backup-20260725-113543/training_pairs.jsonl
"""
import argparse
import datetime
import hashlib
import json
import os
import random
import re
import statistics
import sys
import unicodedata
from pathlib import Path

# Reuse the shared, already-debugged typedstream decoder (see
# scripts/persona_style_card.py for the same import pattern and the
# 2026-07 bugfix history of hand-rolled copies of this decoder).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
from imessage_text import decode_attributed_body  # noqa: E402

APPLE_EPOCH = datetime.datetime(2001, 1, 1)

DEFAULT_DB = os.path.expanduser("~/Library/Messages/chat.db")
DEFAULT_START = "2026-03-01"
DEFAULT_WINDOW_DAYS = 30
DEFAULT_MIN_N = 100
# A window can satisfy min_n on a handful of days (2026-09-03: the move event
# passed with n=180 spanning 5.1 days). Coverage is part of the contract.
DEFAULT_MIN_COVERED_DAYS = 20.0

# Tapback echo text prefixes (e.g. 'Loved "..."') — these are reaction
# echoes, not something Seth typed, and must not enter the style stats.
# Mirrors scripts/persona_style_card.py's TAPBACK_PREFIXES.
TAPBACK_PREFIXES = (
    "Loved “", "Liked “", "Disliked “", "Laughed at “",
    "Emphasized “", "Questioned “",
)

# --- Fixed small warmth lexicon (documented here, not derived) -------------
# Deliberately small and literal: word-boundary, case-insensitive hits
# against this list, no synonymy/embedding lookup. This is a *proxy*, not a
# sentiment model — it exists so a 30-day window can be compared to another
# 30-day window on the same yardstick, not to score any single message.
WARMTH_LEXICON = (
    "love", "miss", "thank", "thanks", "appreciate", "proud", "glad",
    "sweet", "dear", "hug", "xoxo", "grateful", "care", "sorry",
)

# First-person-plural pronoun set for the formality proxy.
FIRST_PERSON_PLURAL = ("we", "us", "our", "ours", "ourselves")

# Matches both the ASCII apostrophe and the curly right-single-quote
# (U+2019) that iOS autocorrect substitutes -- a real chat.db sample of
# 977 outbound messages had 142 curly vs. 18 straight apostrophes, so
# matching only "'" would undercount contractions by roughly 8x.
_APOS_CHARS = "'’"
_CONTRACTION_RE = re.compile(r"\b\w+[" + _APOS_CHARS + r"](?:t|re|ve|ll|d|s|m)\b", re.IGNORECASE)
_WORD_RE = re.compile(r"[A-Za-z" + _APOS_CHARS + r"]+")


# ---------------------------------------------------------------------------
# Event dates — see docs/plans/2026-09-02-persona-evolution/spec.md for the
# full derivation. Both are LOW confidence (not pinned to +/-7 days).
# ---------------------------------------------------------------------------
EVENTS = {
    "move": {
        "label": "Move to St. Petersburg, FL",
        "date": "2026-07-01",
        "confidence": "LOW (not pinned to +/-7 days)",
        "citations": [
            "~/.human/personas/seth.json:70 core.identity: "
            "'Lives in a waterfront place in St. Petersburg (Tampa Bay area) "
            "... before moving to Florida'",
            "~/.human/personas/seth.json:47-50 core.life_events[0]: "
            "{description: 'moved to the waterfront place in st pete', "
            "state: 'completed', as_of: '2026-07-28'} (as_of is when the "
            "note was last confirmed true, not the move date)",
            "~/.human/personas/seth.json.bak-fact-refresh2-20260726-045551 "
            "core.identity (timestamp 2026-07-26T04:55:51): 'Lives in St. "
            "Petersburg, Florida (Tampa Bay area) as of July 2026' -- "
            "earliest persona snapshot naming Florida at all",
            "~/.human/personas/seth.json.bak-fact-refresh-20260726-044442 "
            "core.identity (timestamp 2026-07-26T04:44:42, 11 minutes "
            "earlier): no Florida / St. Petersburg mention -- the persona's "
            "own fact record still carried the pre-move description here",
        ],
        "note": (
            "'as of July 2026' is the only real-world anchor; 2026-07-01 is "
            "the first day consistent with that anchor, chosen as the "
            "best-supported single point estimate per the task's ±7-day "
            "rule (we could not do better than a ~1-month window)."
        ),
    },
    "job": {
        "label": "Employer change: Vanguard -> Raymond James",
        "date": "2026-07-26",
        "confidence": "LOW (not pinned to +/-7 days)",
        "citations": [
            "~/.human/personas/seth.json:70 core.identity: '...Chief "
            "Architect at Raymond James in St. Petersburg, Florida "
            "(started summer 2026, after leaving Vanguard)'",
            "~/.human/personas/seth.json:51-54 core.life_events[1]: "
            "{description: 'left vanguard for raymond james', "
            "state: 'completed', as_of: '2026-07-28'}",
            "~/.human/personas/seth.json.bak-fact-refresh2-20260726-045551 "
            "core.identity (timestamp 2026-07-26T04:55:51): 'Recently left "
            "his role as Chief Architect at Vanguard (summer 2026) and is "
            "in transition to a new role' -- Raymond James not yet named",
            "~/.human/personas/seth.json.bak.pre-typing-quirks-removal-"
            "20260726-081742 core.identity (timestamp 2026-07-26T08:17:42, "
            "~3h20m later): identity already names 'Chief Architect at "
            "Raymond James'",
        ],
        "note": (
            "'summer 2026' spans roughly June-August; 2026-07-26 is the "
            "date the persona's fact record flipped from 'in transition' "
            "to 'Raymond James', the single most concrete dated signal "
            "available, but it is a record-update date, not a confirmed "
            "first-day date."
        ),
    },
}


# ---------------------------------------------------------------------------
# Pure metric functions -- each operates on one message string and nothing
# else, so each is testable with synthetic strings (no chat.db needed).
# ---------------------------------------------------------------------------

def msg_length_chars(text: str) -> int:
    """Character length of a message."""
    return len(text)


def starts_lowercase(text: str):
    """True/False on the first Unicode alphabetic character in the string.

    Leading whitespace, punctuation, digits, and emoji are skipped when
    looking for that first letter. Returns None if the string has no
    alphabetic character at all (caller should exclude those from the
    lowercase-start rate, the same way "n/a" is excluded from any rate).
    """
    for ch in text:
        if ch.isalpha():
            return ch == ch.lower() and ch != ch.upper()
    return None


def terminal_punctuation(text: str) -> str:
    """Classify the final significant punctuation of a message into one of
    {"none", "question", "exclaim", "period", "ellipsis"}.

    Looks at the last non-whitespace character(s); "..." and the unicode
    ellipsis both count as "ellipsis" (checked before single-char period so
    a trailing "..." isn't miscounted as "period" on its final dot alone --
    it is, but ellipsis is checked first below so this comment stays true).
    """
    stripped = text.rstrip()
    if not stripped:
        return "none"
    if stripped.endswith("…") or stripped.endswith("..."):
        return "ellipsis"
    last = stripped[-1]
    if last == "?":
        return "question"
    if last == "!":
        return "exclaim"
    if last == ".":
        return "period"
    return "none"


def is_emoji_char(ch: str) -> bool:
    """Same emoji heuristic as scripts/persona_style_card.py's is_emoji."""
    return unicodedata.category(ch) == "So" or 0x1F000 <= ord(ch) <= 0x1FAFF


def has_emoji(text: str) -> bool:
    return any(is_emoji_char(ch) for ch in text)


def word_tokens(text: str):
    """Lowercased alphabetic word tokens (apostrophes kept, for contraction
    matching), digits and punctuation-only tokens dropped."""
    return [w.lower() for w in _WORD_RE.findall(text)]


def contractions_per_100_words(text: str) -> float:
    words = word_tokens(text)
    if not words:
        return 0.0
    hits = len(_CONTRACTION_RE.findall(text))
    return 100.0 * hits / len(words)


def first_person_plural_per_100_words(text: str) -> float:
    words = word_tokens(text)
    if not words:
        return 0.0
    hits = sum(1 for w in words if w in FIRST_PERSON_PLURAL)
    return 100.0 * hits / len(words)


def warmth_hits_per_100_words(text: str) -> float:
    words = word_tokens(text)
    if not words:
        return 0.0
    hits = sum(1 for w in words if w in WARMTH_LEXICON)
    return 100.0 * hits / len(words)


def compute_features(text: str) -> dict:
    """The full per-message feature vector used by aggregate_window."""
    term = terminal_punctuation(text)
    lc = starts_lowercase(text)
    return {
        "len_chars": msg_length_chars(text),
        "starts_lowercase": (1.0 if lc else 0.0) if lc is not None else None,
        "terminal_none": 1.0 if term == "none" else 0.0,
        "terminal_question": 1.0 if term == "question" else 0.0,
        "terminal_exclaim": 1.0 if term == "exclaim" else 0.0,
        "terminal_period": 1.0 if term == "period" else 0.0,
        "terminal_ellipsis": 1.0 if term == "ellipsis" else 0.0,
        "has_emoji": 1.0 if has_emoji(text) else 0.0,
        "contractions_per_100_words": contractions_per_100_words(text),
        "first_person_plural_per_100_words": first_person_plural_per_100_words(text),
        "warmth_hits_per_100_words": warmth_hits_per_100_words(text),
    }


# The axes reported by this script, in report order. Each maps a feature
# key (from compute_features) to a human label. "starts_lowercase" is
# filtered for None (messages with no alphabetic character) before
# averaging; every other axis is a 0/1 or continuous value for every
# message.
AXES = [
    ("len_chars", "length_chars"),
    ("starts_lowercase", "lowercase_start_rate"),
    ("terminal_none", "no_terminal_punct_rate"),
    ("terminal_question", "question_rate"),
    ("terminal_exclaim", "exclamation_rate"),
    ("has_emoji", "emoji_rate"),
    ("contractions_per_100_words", "formality_contractions_per_100_words"),
    ("first_person_plural_per_100_words", "formality_first_person_plural_per_100_words"),
    ("warmth_hits_per_100_words", "warmth_hits_per_100_words"),
]


# ---------------------------------------------------------------------------
# Bootstrap CI
# ---------------------------------------------------------------------------

def bootstrap_ci(values, n_resamples: int = 2000, confidence: float = 0.95, seed: int = 42):
    """Percentile bootstrap CI. Returns (mean, lo, hi).

    Deterministic (fixed seed) so the same window always reports the same
    CI. Matches the shape of scripts/eval_fidelity_helpers.bootstrap_ci but
    is reimplemented here standalone so this script has zero import
    dependency beyond the shared attributedBody decoder.
    """
    if not values:
        return (0.0, 0.0, 0.0)
    if len(values) == 1:
        return (values[0], values[0], values[0])
    rng = random.Random(seed)
    n = len(values)
    means = []
    for _ in range(n_resamples):
        means.append(sum(values[rng.randrange(n)] for _ in range(n)) / n)
    means.sort()
    alpha = (1 - confidence) / 2
    lo_idx = int(alpha * n_resamples)
    hi_idx = min(n_resamples - 1, int((1 - alpha) * n_resamples))
    return (statistics.mean(values), means[lo_idx], means[hi_idx])


# ---------------------------------------------------------------------------
# Window aggregation
# ---------------------------------------------------------------------------

def aggregate_window(texts, n_resamples: int = 2000, seed: int = 42) -> dict:
    """texts: list[str]. Returns {"n": int, "axes": {axis_name: {mean, ci_lo,
    ci_hi}}}. Pure function -- no chat.db, no I/O."""
    n = len(texts)
    feats = [compute_features(t) for t in texts]
    axes_out = {}
    for key, label in AXES:
        vals = [f[key] for f in feats if f[key] is not None]
        mean, lo, hi = bootstrap_ci(vals, n_resamples=n_resamples, seed=seed)
        axes_out[label] = {
            "mean": mean,
            "ci_lo": lo,
            "ci_hi": hi,
            "n": len(vals),
        }
    return {"n": n, "axes": axes_out}


def bucket_by_window(messages, event_date: datetime.date, window_days: int):
    """messages: list[(datetime, str)]. Returns (pre_texts, post_texts) for
    the [event-window, event) and [event, event+window) half-open windows."""
    event_dt = datetime.datetime.combine(event_date, datetime.time.min)
    pre_lo = event_dt - datetime.timedelta(days=window_days)
    post_hi = event_dt + datetime.timedelta(days=window_days)
    pre = [t for ts, t in messages if pre_lo <= ts < event_dt]
    post = [t for ts, t in messages if event_dt <= ts < post_hi]
    return pre, post


def delta_report(pre_agg: dict, post_agg: dict) -> dict:
    """Per-axis delta = post.mean - pre.mean, plus a CI-based
    'moved_beyond_ci' flag: True when the post mean falls outside the pre
    window's own 95% CI (or vice versa) -- i.e. the shift is bigger than the
    window's own sampling noise. This is NOT a t-test; it is the
    conservative, dependency-free heuristic used elsewhere in this repo's
    eval scripts (see .claude/rules/no-number-without-a-measurement.md on
    not overclaiming precision this script doesn't have)."""
    out = {}
    for _, label in AXES:
        pa, qa = pre_agg["axes"][label], post_agg["axes"][label]
        delta = qa["mean"] - pa["mean"]
        moved = (qa["mean"] < pa["ci_lo"] or qa["mean"] > pa["ci_hi"] or
                 pa["mean"] < qa["ci_lo"] or pa["mean"] > qa["ci_hi"])
        out[label] = {
            "pre": pa,
            "post": qa,
            "delta": delta,
            "moved_beyond_ci": moved,
        }
    return out


# ---------------------------------------------------------------------------
# --source: second-store reader, de-dup, sampling-frame filter, coverage
# ---------------------------------------------------------------------------

# Timezone the export's naive timestamps were written in. None = this
# machine's current local zone (what datetime.fromtimestamp used when
# scripts/extract_imessage_pairs.py ran). Tests pin a fixed offset.
EXPORT_TZ = None

# scripts/extract_imessage_pairs.py MIN_REPLY_LENGTH -- the export drops
# Seth messages shorter than this, so chat.db rows get the same floor when
# the two stores are mixed.
EXPORT_MIN_REPLY_LENGTH = 2


def local_naive_to_utc_naive(dt: datetime.datetime, tz=None) -> datetime.datetime:
    """Reinterpret a naive local-time datetime as UTC naive, matching the
    chat.db path (_apple_ns_to_dt is UTC naive)."""
    if tz is None:
        aware = dt.astimezone()  # naive -> system local
    else:
        aware = dt.replace(tzinfo=tz)
    return aware.astimezone(datetime.timezone.utc).replace(tzinfo=None)


def _export_record_seth_text(rec: dict):
    """(local_iso_timestamp, text) for one export record, or raise
    ValueError if the record has no Seth-authorship provenance."""
    if "messages" in rec and isinstance(rec.get("metadata"), dict) and "timestamp" in rec["metadata"]:
        last = rec["messages"][-1] if rec["messages"] else None
        if not isinstance(last, dict) or last.get("role") != "assistant":
            raise ValueError("training_pairs record whose last turn is not the assistant (is_from_me=1) turn")
        return rec["metadata"]["timestamp"], last.get("content")
    if "seth_reply" in rec and "timestamp" in rec:
        return rec["timestamp"], rec["seth_reply"]
    raise ValueError(
        "unrecognized export shape (need training_pairs messages/metadata.timestamp "
        "or ground_truth seth_reply/timestamp); DPO/memory.db rows carry no Seth provenance"
    )


def read_export_jsonl(path: str, tz=None):
    """Read a Seth-authored export. Returns list[(utc_naive_datetime, str)],
    tapback echoes and blank texts dropped, sorted by time."""
    out = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            ts_local, text = _export_record_seth_text(json.loads(line))
            if not text or not text.strip():
                continue
            text = text.strip()
            if text.startswith(TAPBACK_PREFIXES):
                continue
            ts = local_naive_to_utc_naive(datetime.datetime.fromisoformat(ts_local), tz=tz)
            out.append((ts, text))
    out.sort(key=lambda r: r[0])
    return out


def dedup_key(ts: datetime.datetime, text: str):
    """(timestamp truncated to the second, sha256 of stripped text). Second
    precision because the export carries microseconds only sometimes."""
    return (ts.replace(microsecond=0).isoformat(),
            hashlib.sha256(text.strip().encode("utf-8")).hexdigest())


def merge_sources(primary, extras):
    """primary: list[(dt, str)] kept wholesale. extras: list[(label,
    list[(dt, str)])]; a row is added only if its dedup_key is unseen.
    Returns (merged sorted by time, per-extra stats)."""
    seen = {dedup_key(ts, t) for ts, t in primary}
    merged = list(primary)
    stats = []
    for label, rows in extras:
        added = 0
        for ts, t in rows:
            k = dedup_key(ts, t)
            if k in seen:
                continue
            seen.add(k)
            merged.append((ts, t))
            added += 1
        stats.append({"path": label, "rows": len(rows), "added": added, "duplicates": len(rows) - added})
    merged.sort(key=lambda r: r[0])
    return merged, stats


def export_frame_filter(messages, min_len: int = EXPORT_MIN_REPLY_LENGTH):
    """Apply the export's length floor to chat.db rows. Returns (kept, n_dropped)."""
    kept = [(ts, t) for ts, t in messages if len(t) >= min_len]
    return kept, len(messages) - len(kept)


def window_coverage(messages, lo: datetime.datetime, hi: datetime.datetime) -> dict:
    """First/last timestamp actually present in [lo, hi) and the span in
    days, so a short-coverage window is never mistaken for a full one."""
    ts = [t for t, _ in messages if lo <= t < hi]
    if not ts:
        return {"first": None, "last": None, "covered_days": 0.0}
    first, last = min(ts), max(ts)
    return {
        "first": first.isoformat(),
        "last": last.isoformat(),
        "covered_days": round((last - first).total_seconds() / 86400.0, 1),
    }


# ---------------------------------------------------------------------------
# chat.db access -- read-only, no content leaves this process
# ---------------------------------------------------------------------------

def _apple_ns_bounds(start: datetime.datetime, end: datetime.datetime):
    lo = int((start - APPLE_EPOCH).total_seconds() * 1_000_000_000)
    hi = int((end - APPLE_EPOCH).total_seconds() * 1_000_000_000)
    return lo, hi


def _apple_ns_to_dt(ns: int) -> datetime.datetime:
    return APPLE_EPOCH + datetime.timedelta(seconds=ns / 1_000_000_000)


def fetch_outbound_messages(db_path: str, start: datetime.datetime, end: datetime.datetime):
    """Read-only fetch of Seth's own (is_from_me=1) typed texts in
    [start, end). Tapback echoes are excluded. Returns list[(datetime,
    str)]. Opens the DB read-only + immutable so this process can never
    mutate chat.db, and closes it before returning."""
    import sqlite3

    uri = f"file:{db_path}?mode=ro&immutable=1"
    con = sqlite3.connect(uri, uri=True)
    try:
        lo, hi = _apple_ns_bounds(start, end)
        rows = con.execute(
            """
            SELECT date, text, attributedBody, COALESCE(associated_message_type, 0)
            FROM message
            WHERE is_from_me = 1
              AND date >= ? AND date < ?
              AND (text IS NOT NULL OR attributedBody IS NOT NULL)
            ORDER BY date
            """,
            (lo, hi),
        ).fetchall()
    finally:
        con.close()

    out = []
    for date_ns, text, blob, assoc in rows:
        if text is None or not text.strip():
            text = decode_attributed_body(blob) if blob is not None else None
        if not text:
            continue
        text = text.strip()
        if 2000 <= assoc <= 2005 or text.startswith(TAPBACK_PREFIXES):
            continue
        out.append((_apple_ns_to_dt(date_ns), text))
    return out


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _parse_date(s: str) -> datetime.date:
    return datetime.datetime.strptime(s, "%Y-%m-%d").date()


def run(args) -> int:
    if args.event == "both":
        event_names = ["move", "job"]
    elif args.event == "none":
        event_names = []
    else:
        event_names = [args.event]

    start_dt = datetime.datetime.combine(_parse_date(args.start), datetime.time.min)
    end_dt = (
        datetime.datetime.combine(_parse_date(args.end), datetime.time.min)
        if args.end
        else datetime.datetime.now()
    )

    # --trailing-days: windowed re-derivation mode (AC-7.2). Overrides --start
    # with end_dt - N days; reuses start_dt/end_dt for fetch_outbound_messages,
    # --source filtering, and the report's own "range" field verbatim -- no
    # separate fetch/filter path.
    #
    # This reuse is exactly why --trailing-days and --full-range are mutually
    # exclusive (finding 1, 2026-09-05 critic re-open): --full-range also
    # summarizes over that same fetch_outbound_messages(start_dt, end_dt)
    # call, so if both flags were allowed together, --trailing-days's
    # override of start_dt would silently narrow --full-range's "whole
    # --start/--end range" summary down to the trailing window with no
    # signal to the caller (verified live: --full-range --trailing-days 30
    # returned full_range_summary.n=20 instead of the true full-range n=160)
    # -- exactly the kind of unmeasured, mislabeled number
    # .claude/rules/no-number-without-a-measurement.md exists to prevent.
    # Fetching the trailing window separately (without mutating start_dt)
    # was considered and rejected: it would duplicate the --source
    # filter/merge path above for a second, differently-scoped `messages`
    # list, doubling the surface this refusal contract has to hold for a
    # combination nothing currently schedules (design doc: nobody runs
    # --full-range today; --trailing-days is the sanctioned nightly command).
    # Refusing the combination outright is simpler and cannot silently
    # mislabel a number.
    trailing_days = getattr(args, "trailing_days", None)
    if trailing_days is not None and getattr(args, "full_range", False):
        sys.stderr.write(
            "ERROR: --trailing-days and --full-range are mutually exclusive "
            "(both would summarize the fetch_outbound_messages(start_dt, "
            "end_dt) range, and --trailing-days overrides start_dt, which "
            "would silently narrow --full-range's window to the trailing "
            "window). Run them in separate invocations.\n"
        )
        return 2
    if trailing_days is not None:
        if args.start != DEFAULT_START:
            sys.stderr.write(
                "NOTE: --trailing-days=%d overrides --start=%s; using a "
                "start computed from --end (or now) minus %d days instead.\n"
                % (trailing_days, args.start, trailing_days)
            )
        start_dt = end_dt - datetime.timedelta(days=trailing_days)

    if args.explain_dates:
        print(json.dumps({name: EVENTS[name] for name in event_names}, indent=2))
        return 0

    messages = fetch_outbound_messages(args.db, start_dt, end_dt)
    primary_n = len(messages)
    min_cov = float(getattr(args, "min_covered_days", 0.0) or 0.0)

    sources = list(getattr(args, "source", None) or [])
    source_stats = []
    frame_dropped = 0
    if sources:
        messages, frame_dropped = export_frame_filter(messages)
        extras = []
        for path in sources:
            rows = [(ts, t) for ts, t in read_export_jsonl(path, tz=EXPORT_TZ) if start_dt <= ts < end_dt]
            extras.append((path, rows))
        messages, source_stats = merge_sources(messages, extras)

    report = {
        "db": args.db,
        "range": {"start": start_dt.date().isoformat(), "end": end_dt.date().isoformat()},
        "window_days": args.window_days,
        "min_n": args.min_n,
        "min_covered_days": min_cov,
        "primary_outbound_messages_in_range": primary_n,
        "total_outbound_messages_in_range": len(messages),
        "events": {},
    }
    if sources:
        report["sources"] = source_stats
        report["primary_rows_dropped_by_export_frame"] = frame_dropped
        report["source_caveats"] = [
            "export rows are Seth-authored via chat.db is_from_me=1 "
            "(scripts/extract_imessage_pairs.py); DPO/memory.db stores were rejected",
            "export timestamps converted local->UTC before windowing and de-dup",
            f"chat.db rows filtered to the export's len>={EXPORT_MIN_REPLY_LENGTH} floor",
            "export omits Seth messages that open a conversation window with no prior "
            "context; that exclusion cannot be reproduced on the chat.db side",
        ]

    insufficient = False
    for name in event_names:
        ev = EVENTS[name]
        event_date = _parse_date(ev["date"])
        event_dt = datetime.datetime.combine(event_date, datetime.time.min)
        pre_texts, post_texts = bucket_by_window(messages, event_date, args.window_days)
        pre_n, post_n = len(pre_texts), len(post_texts)

        entry = {
            "label": ev["label"],
            "event_date": ev["date"],
            "date_confidence": ev["confidence"],
            "date_note": ev["note"],
            "date_citations": ev["citations"],
            "pre_window": {
                "start": (event_date - datetime.timedelta(days=args.window_days)).isoformat(),
                "end": event_date.isoformat(),
                "n": pre_n,
                "coverage": window_coverage(messages, event_dt - datetime.timedelta(days=args.window_days), event_dt),
            },
            "post_window": {
                "start": event_date.isoformat(),
                "end": (event_date + datetime.timedelta(days=args.window_days)).isoformat(),
                "n": post_n,
                "coverage": window_coverage(messages, event_dt, event_dt + datetime.timedelta(days=args.window_days)),
            },
        }

        pre_cov = entry["pre_window"]["coverage"]["covered_days"]
        post_cov = entry["post_window"]["coverage"]["covered_days"]
        short_coverage = [
            f"{side} covered_days={cov} < min_covered_days={min_cov}"
            for side, cov in (("pre", pre_cov), ("post", post_cov)) if cov < min_cov
        ]
        if pre_n < args.min_n or post_n < args.min_n or short_coverage:
            entry["status"] = "INSUFFICIENT_DATA"
            entry["reason"] = (
                f"pre_window n={pre_n}, post_window n={post_n}; both must be "
                f">= min_n={args.min_n}"
                + ("; " + "; ".join(short_coverage) if short_coverage else "")
                + ". Refusing to compute stats for this event per the "
                "no-fabricated-numbers contract."
            )
            insufficient = True
        else:
            pre_agg = aggregate_window(pre_texts, n_resamples=args.n_resamples, seed=args.seed)
            post_agg = aggregate_window(post_texts, n_resamples=args.n_resamples, seed=args.seed)
            entry["status"] = "OK"
            entry["axes"] = delta_report(pre_agg, post_agg)

        report["events"][name] = entry

    if args.full_range:
        fr_n = len(messages)
        fr_texts = [t for _, t in messages]
        fr_entry = {"n": fr_n}
        if fr_n < args.min_n:
            fr_entry["status"] = "INSUFFICIENT_DATA"
            fr_entry["reason"] = f"n={fr_n} < min_n={args.min_n}"
            insufficient = True
        else:
            fr_entry["status"] = "OK"
            fr_entry["axes"] = aggregate_window(
                fr_texts, n_resamples=args.n_resamples, seed=args.seed
            )["axes"]
        report["full_range_summary"] = fr_entry

    if trailing_days is not None:
        tw_n = len(messages)
        tw_texts = [t for _, t in messages]
        tw_coverage = window_coverage(messages, start_dt, end_dt)
        tw_entry = {
            "requested_days": trailing_days,
            "window": {"start": start_dt.date().isoformat(), "end": end_dt.date().isoformat()},
            "n": tw_n,
            "coverage": tw_coverage,
        }
        tw_reasons = []
        if tw_n < args.min_n:
            tw_reasons.append(f"n={tw_n} < min_n={args.min_n}")
        if tw_coverage["covered_days"] < min_cov:
            tw_reasons.append(f"covered_days={tw_coverage['covered_days']} < min_covered_days={min_cov}")
        if tw_reasons:
            tw_entry["status"] = "INSUFFICIENT_DATA"
            tw_entry["reason"] = (
                "; ".join(tw_reasons) + ". Refusing to compute stats for the "
                "trailing window per the no-fabricated-numbers contract."
            )
            insufficient = True
        else:
            tw_entry["status"] = "OK"
            tw_entry["axes"] = aggregate_window(
                tw_texts, n_resamples=args.n_resamples, seed=args.seed
            )["axes"]
        report["trailing_window_summary"] = tw_entry

    report["overall_status"] = "INSUFFICIENT_DATA" if insufficient else "OK"

    print(json.dumps(report, indent=2))

    if insufficient:
        sys.stderr.write(
            "REFUSED: at least one window had n < min_n or covered_days < "
            "min_covered_days; wrote nothing to --out. See the "
            "'events[*].reason' fields above.\n"
        )
        return 1

    if args.out:
        Path(args.out).write_text(json.dumps(report, indent=2) + "\n")

    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", default=DEFAULT_DB, help="path to chat.db (default: %(default)s)")
    p.add_argument("--start", default=DEFAULT_START, help="overall range start, YYYY-MM-DD (default: %(default)s)")
    p.add_argument("--end", default=None, help="overall range end, YYYY-MM-DD (default: today)")
    p.add_argument("--window-days", type=int, default=DEFAULT_WINDOW_DAYS, help="pre/post window size in days (default: %(default)s)")
    p.add_argument("--min-n", type=int, default=DEFAULT_MIN_N, help="minimum messages per window, else refuse (default: %(default)s)")
    p.add_argument("--min-covered-days", type=float, default=DEFAULT_MIN_COVERED_DAYS,
                   help="minimum span (days) between the first and last message actually present in each "
                        "window, else refuse; 0 disables (default: %(default)s)")
    p.add_argument("--event", choices=["move", "job", "both", "none"], default="both", help="which event(s) to analyze ('none' skips event analysis, useful with --full-range)")
    p.add_argument("--full-range", action="store_true", help="also report an aggregate style summary over the whole --start/--end range (no before/after split)")
    p.add_argument("--n-resamples", type=int, default=2000, help="bootstrap resamples (default: %(default)s)")
    p.add_argument("--seed", type=int, default=42, help="bootstrap RNG seed (default: %(default)s)")
    p.add_argument("--out", default=None, help="write the JSON report here on success ONLY (never on refusal)")
    p.add_argument("--source", action="append", default=None, metavar="JSONL",
                   help="second store of Seth-authored texts (training_pairs.jsonl or ground_truth.jsonl "
                        "from scripts/extract_imessage_pairs.py); repeatable; de-duplicated against chat.db "
                        "by (timestamp, text hash)")
    p.add_argument("--explain-dates", action="store_true", help="print the event-date citations and exit, no DB access")
    p.add_argument("--trailing-days", type=int, default=None, metavar="N",
                   help="windowed re-derivation mode: re-derive all 9 axes over the trailing N days "
                        "ending at --end (or now), overriding --start; requires --event none "
                        "(mutually exclusive with event pre/post analysis, which uses --window-days) "
                        "and mutually exclusive with --full-range (both summarize the same fetched "
                        "range, and --trailing-days overriding --start would silently narrow "
                        "--full-range's window); reports true coverage.covered_days alongside "
                        "requested_days, never conflating the two "
                        "(see .claude/rules/no-number-without-a-measurement.md)")
    args = p.parse_args(argv)
    if args.trailing_days is not None and args.event != "none":
        p.error(
            "--trailing-days requires --event none (event pre/post windows use "
            "--window-days, not --trailing-days); pass --event none to use "
            "--trailing-days, or drop --trailing-days to analyze events"
        )
    if args.trailing_days is not None and args.full_range:
        p.error(
            "--trailing-days and --full-range are mutually exclusive: both "
            "summarize the fetch_outbound_messages(start_dt, end_dt) range, "
            "and --trailing-days overrides start_dt, which would silently "
            "narrow --full-range's reported window from --start/--end down "
            "to the trailing window. Run them in separate invocations."
        )
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
