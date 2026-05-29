#!/usr/bin/env python3
"""
Wave 7 / W7-1 — voice data-corpus audit.

Answers the question the whole fidelity effort has been guessing at: *do we
have enough, diverse-enough real text to fine-tune a convincing voice?* Voice
fidelity is data-bound — this characterizes the binding constraint instead of
optimizing architecture in the dark.

Reads the h-uman conversation DB (default ~/.human/memory.db, table
`messages(session_id, role, content[, id])`) and reports:
  - volume: total / user / assistant message counts
  - structure: sessions, adjacent user→assistant candidate example pairs
  - signal quality: assistant-message length distribution, very-short count
  - diversity: type-token ratio (unique tokens / total) on assistant text
  - per-session breakdown (proxy for per-channel/contact)
  - a data-viability verdict with named gaps

The verdict is heuristic and intentionally conservative; thresholds are
documented in data_viability_verdict() and pinned by tests. No network, stdlib
only. Run on your real machine — this script can't see your DB from CI.

Usage:
  scripts/voice_data_audit.py [--db ~/.human/memory.db] [--output-json audit.json]
"""

import argparse
import json
import sqlite3
import statistics
import sys
from pathlib import Path

DEFAULT_DB = Path.home() / ".human" / "memory.db"

# Viability thresholds (documented + tested). Grounded in the survey finding
# that single-person voice fine-tuning needs ~200-750 well-formed samples;
# below ~50 the adapter amplifies a thin slice and plateaus regardless of
# architecture.
VIABLE_ASSISTANT_MIN = 300
THIN_ASSISTANT_MIN = 50
MIN_SESSIONS = 5
SHORT_MSG_CHARS = 10  # assistant messages shorter than this carry little voice
LOW_DIVERSITY_TTR = 0.15  # type-token ratio below this = repetitive/templated


def _tokens(text):
    return [t for t in "".join(c.lower() if (c.isalnum() or c.isspace()) else " " for c in text).split()]


def audit_rows(rows):
    """Pure: compute corpus stats from rows of (session_id, role, content).

    Rows are assumed in temporal order within a session (the caller orders by
    session_id, id). Returns a stats dict.
    """
    total = len(rows)
    user_n = assistant_n = 0
    sessions = {}
    pairs = 0
    pending_user = {}  # session_id -> True if last seen row was a user turn
    assistant_lens = []
    all_tokens = []
    uniq_tokens = set()

    prev_session = None
    for session_id, role, content in rows:
        role = (role or "").lower()
        content = content or ""
        if session_id != prev_session:
            pending_user.pop(prev_session, None)
            prev_session = session_id
        sessions.setdefault(session_id, {"user": 0, "assistant": 0})
        if role == "user":
            user_n += 1
            sessions[session_id]["user"] += 1
            pending_user[session_id] = True
        elif role == "assistant":
            assistant_n += 1
            sessions[session_id]["assistant"] += 1
            assistant_lens.append(len(content))
            toks = _tokens(content)
            all_tokens.extend(toks)
            uniq_tokens.update(toks)
            if pending_user.get(session_id):
                pairs += 1
                pending_user[session_id] = False

    short_assistant = sum(1 for n in assistant_lens if n < SHORT_MSG_CHARS)
    ttr = (len(uniq_tokens) / len(all_tokens)) if all_tokens else 0.0
    return {
        "total_messages": total,
        "user_messages": user_n,
        "assistant_messages": assistant_n,
        "sessions": len(sessions),
        "candidate_pairs": pairs,
        "assistant_len_min": min(assistant_lens) if assistant_lens else 0,
        "assistant_len_mean": round(statistics.mean(assistant_lens), 1) if assistant_lens else 0.0,
        "assistant_len_median": int(statistics.median(assistant_lens)) if assistant_lens else 0,
        "assistant_len_max": max(assistant_lens) if assistant_lens else 0,
        "short_assistant_msgs": short_assistant,
        "type_token_ratio": round(ttr, 4),
        "sessions_with_pairs": sum(1 for s in sessions.values() if s["user"] and s["assistant"]),
    }


def data_viability_verdict(stats):
    """Pure: classify corpus as viable / thin / starved with named gaps."""
    a = stats["assistant_messages"]
    reasons = []
    if a < THIN_ASSISTANT_MIN:
        verdict = "starved"
        reasons.append(
            f"only {a} assistant messages (<{THIN_ASSISTANT_MIN}); a voice adapter would "
            f"overfit a thin slice. Target ~{VIABLE_ASSISTANT_MIN}+."
        )
    elif a < VIABLE_ASSISTANT_MIN:
        verdict = "thin"
        reasons.append(
            f"{a} assistant messages (between {THIN_ASSISTANT_MIN} and {VIABLE_ASSISTANT_MIN}); "
            f"trainable but expect a low ceiling. More + more-diverse data lifts it."
        )
    else:
        verdict = "viable"

    if stats["sessions"] < MIN_SESSIONS:
        reasons.append(
            f"only {stats['sessions']} distinct sessions (<{MIN_SESSIONS}); homogeneous data "
            f"amplifies one register. Diversity across contacts/channels matters."
        )
        if verdict == "viable":
            verdict = "thin"
    if stats["assistant_messages"] and stats["type_token_ratio"] < LOW_DIVERSITY_TTR:
        reasons.append(
            f"type-token ratio {stats['type_token_ratio']} (<{LOW_DIVERSITY_TTR}); text is "
            f"repetitive/templated — low distinctive-voice signal."
        )
    if stats["assistant_messages"]:
        short_frac = stats["short_assistant_msgs"] / stats["assistant_messages"]
        if short_frac > 0.5:
            reasons.append(
                f"{round(short_frac*100)}% of assistant messages are <{SHORT_MSG_CHARS} chars "
                f"(acks/emoji); few carry real voice."
            )
    if verdict == "viable" and not reasons:
        reasons.append("sufficient volume + diversity for a first voice adapter.")
    return verdict, reasons


def read_messages(db_path):
    """Read (session_id, role, content) rows ordered as the extractor does.
    Returns [] if the table/columns aren't present (reports gracefully)."""
    conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        cols = {r[1] for r in conn.execute("PRAGMA table_info(messages)")}
        if not {"session_id", "role", "content"} <= cols:
            return None
        order = "session_id, id" if "id" in cols else "session_id, rowid"
        return list(conn.execute(f"SELECT session_id, role, content FROM messages ORDER BY {order}"))
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser(description="Audit the voice training corpus (W7-1)")
    ap.add_argument("--db", type=Path, default=DEFAULT_DB)
    ap.add_argument("--output-json", type=Path, default=None)
    args = ap.parse_args()

    if not args.db.exists():
        print(f"[error] conversation DB not found: {args.db}", file=sys.stderr)
        return 1
    rows = read_messages(args.db)
    if rows is None:
        print(f"[error] {args.db} has no messages(session_id, role, content) table", file=sys.stderr)
        return 1

    stats = audit_rows(rows)
    verdict, reasons = data_viability_verdict(stats)
    report = {"db": str(args.db), "verdict": verdict, "reasons": reasons, "stats": stats}

    print(json.dumps(report, indent=2))
    print(f"\n=== VOICE DATA VERDICT: {verdict.upper()} ===", file=sys.stderr)
    for r in reasons:
        print(f"  - {r}", file=sys.stderr)
    if args.output_json:
        args.output_json.write_text(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
