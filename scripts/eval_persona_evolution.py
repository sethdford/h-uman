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
--min-n messages (default 100), this script:
  - prints a diagnostic to stdout tagged "status": "INSUFFICIENT_DATA"
  - exits with a non-zero status
  - writes NOTHING to --out, regardless of whether --out was given

A number this script did not measure is worse than no number.

Usage
-----
    python3 scripts/eval_persona_evolution.py --event both
    python3 scripts/eval_persona_evolution.py --event job --out results.json
    python3 scripts/eval_persona_evolution.py --explain-dates
"""
import argparse
import datetime
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

    if args.explain_dates:
        print(json.dumps({name: EVENTS[name] for name in event_names}, indent=2))
        return 0

    messages = fetch_outbound_messages(args.db, start_dt, end_dt)

    report = {
        "db": args.db,
        "range": {"start": start_dt.date().isoformat(), "end": end_dt.date().isoformat()},
        "window_days": args.window_days,
        "min_n": args.min_n,
        "total_outbound_messages_in_range": len(messages),
        "events": {},
    }

    insufficient = False
    for name in event_names:
        ev = EVENTS[name]
        event_date = _parse_date(ev["date"])
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
            },
            "post_window": {
                "start": event_date.isoformat(),
                "end": (event_date + datetime.timedelta(days=args.window_days)).isoformat(),
                "n": post_n,
            },
        }

        if pre_n < args.min_n or post_n < args.min_n:
            entry["status"] = "INSUFFICIENT_DATA"
            entry["reason"] = (
                f"pre_window n={pre_n}, post_window n={post_n}; both must be "
                f">= min_n={args.min_n}. Refusing to compute stats for this "
                "event per the no-fabricated-numbers contract."
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

    report["overall_status"] = "INSUFFICIENT_DATA" if insufficient else "OK"

    print(json.dumps(report, indent=2))

    if insufficient:
        sys.stderr.write(
            "REFUSED: at least one window had n < min_n; wrote nothing to "
            "--out. See the 'events[*].reason' fields above.\n"
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
    p.add_argument("--event", choices=["move", "job", "both", "none"], default="both", help="which event(s) to analyze ('none' skips event analysis, useful with --full-range)")
    p.add_argument("--full-range", action="store_true", help="also report an aggregate style summary over the whole --start/--end range (no before/after split)")
    p.add_argument("--n-resamples", type=int, default=2000, help="bootstrap resamples (default: %(default)s)")
    p.add_argument("--seed", type=int, default=42, help="bootstrap RNG seed (default: %(default)s)")
    p.add_argument("--out", default=None, help="write the JSON report here on success ONLY (never on refusal)")
    p.add_argument("--explain-dates", action="store_true", help="print the event-date citations and exit, no DB access")
    args = p.parse_args(argv)
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
