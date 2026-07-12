#!/usr/bin/env python3
"""persona_style_card.py — derive a MEASURED style card from the user's real
outbound iMessages, and diff it against the hand-written persona rules.

Why: the 2026-07-12 gap analysis found the authored rules in seth.json
contradict the corpus (e.g. "All lowercase unless SHOUTING" vs a measured 2%
lowercase-start rate — iPhone autocapitalize). The model was faithfully
imitating a wrong description. This script replaces authored guesses with
measured distributions, for three consumers:

  1. the persona prompt builder (style rules grounded in data)
  2. deterministic shape governors (terminal-punct strip rate, ?-cap, splitter)
  3. adapter training (v5+ corpus filtering / eval targets)

Typed messages only — tapback echoes (associated_message_type != 0) are
excluded from text stats and surfaced separately as reaction_rate, which is
its own style signal.

Usage:
    python3 scripts/persona_style_card.py [--db ~/Library/Messages/chat.db]
        [--days 180] [--out ~/.human/personas/seth.style-card.json]
        [--persona ~/.human/personas/seth.json] [--selftest]

The card lands OUTSIDE the repo by default (it derives from private texts).
"""
import argparse
import json
import os
import re
import sqlite3
import statistics
import sys
import unicodedata
from collections import Counter

APPLE_EPOCH_SQL = "m.date/1000000000 + strftime('%s','2001-01-01')"

TAPBACK_PREFIXES = ("Loved “", "Liked “", "Disliked “", "Laughed at “",
                    "Emphasized “", "Questioned “")


def extract_text_from_attributed_body(blob):
    """Decode text from an NSAttributedString blob. Copied verbatim from
    scripts/extract_imessage_pairs.py (that module has no import guard —
    importing it would execute its extraction run)."""
    idx = blob.find(b"NSString")
    if idx < 0:
        return None
    start = blob.find(b"+", idx)
    if start < 0:
        return None
    start += 1
    end = blob.find(b"\x86", start)
    if end < 0:
        end = start + 2000
    raw = blob[start:end]
    try:
        text = raw.decode("utf-8", errors="ignore").strip()
    except Exception:
        return None
    text = re.sub(r"^[\x00-\x1f]+", "", text)
    return text if len(text) > 1 else None


def fetch_messages(db_path, days):
    """(typed, reactions) from chat.db. typed = list[(ts, chat_key, text)].

    No handle JOIN — from-me rows usually carry handle_id=0, which a JOIN
    silently drops (~95% of outbound). Conversation identity for burst stats
    comes from chat_message_join. text falls back to attributedBody decode
    (modern macOS stores most message text only there)."""
    con = sqlite3.connect(f"file:{os.path.expanduser(db_path)}?mode=ro", uri=True)
    rows = con.execute(f"""
        SELECT {APPLE_EPOCH_SQL} AS ts,
               COALESCE(c.chat_identifier, 'unknown'),
               m.text, m.attributedBody,
               COALESCE(m.associated_message_type, 0)
        FROM message m
        LEFT JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        LEFT JOIN chat c ON c.ROWID = cmj.chat_id
        WHERE m.is_from_me = 1
          AND (m.text IS NOT NULL OR m.attributedBody IS NOT NULL)
          AND {APPLE_EPOCH_SQL} > strftime('%s','now') - 86400 * ?
        ORDER BY ts""", (days,)).fetchall()
    con.close()
    typed, reactions = [], 0
    for ts, chat_key, text, blob, assoc in rows:
        if text is None and blob is not None:
            text = extract_text_from_attributed_body(blob)
        if not text:
            continue
        # associated_message_type 2000-2005 = tapbacks; the text is an echo
        # ("Loved “...”"), not something the user typed. Belt-and-braces:
        # also match the echo prefix for rows where assoc is 0 on older OSes.
        if 2000 <= assoc <= 2005 or text.startswith(TAPBACK_PREFIXES):
            reactions += 1
            continue
        typed.append((ts, chat_key, text))
    return typed, reactions


def is_emoji(ch):
    return unicodedata.category(ch) == "So" or 0x1F000 <= ord(ch) <= 0x1FAFF


def burst_lengths(typed, window_sec=60):
    """Consecutive typed messages to the same handle within window_sec."""
    bursts, cur = [], 1
    for i in range(1, len(typed)):
        same = typed[i][1] == typed[i - 1][1]
        close = typed[i][0] - typed[i - 1][0] <= window_sec
        if same and close:
            cur += 1
        else:
            bursts.append(cur)
            cur = 1
    bursts.append(cur)
    return bursts


def build_card(typed, reactions):
    texts = [t for _, _, t in typed]
    n = len(texts)
    if n == 0:
        raise SystemExit("no typed messages in window")
    lens = sorted(len(t) for t in texts)

    def pct(pred):
        return round(sum(1 for t in texts if pred(t)) / n, 3)

    def q(p):
        return lens[min(n - 1, int(p * n))]

    terminal = Counter()
    for t in texts:
        last = t.rstrip()[-1] if t.rstrip() else ""
        terminal["?" if last == "?" else "!" if last == "!" else
                 "." if last == "." else "…" if last == "…" else "none"] += 1

    openers = Counter(re.split(r"\s+", t.strip())[0].lower().strip(".,!?")
                      for t in texts if t.strip())
    emoji = Counter(ch for t in texts for ch in t if is_emoji(ch))
    bursts = burst_lengths(typed)

    return {
        "generated_from": {"typed_messages": n, "reactions": reactions},
        "reaction_rate": round(reactions / (reactions + n), 3),
        "length": {
            "median": q(0.5), "p25": q(0.25), "p75": q(0.75), "p90": q(0.9),
            "under_20_chars": pct(lambda t: len(t) < 20),
            "over_120_chars": pct(lambda t: len(t) > 120),
        },
        "terminal_punctuation": {
            k: round(v / n, 3) for k, v in sorted(terminal.items(),
                                                  key=lambda kv: -kv[1])
        },
        "case": {
            "starts_lowercase": pct(lambda t: t[:1].islower()),
            "has_all_caps_word": pct(
                lambda t: any(w.isupper() and len(w) >= 3
                              for w in re.split(r"\s+", t))),
        },
        "question_ending_rate": pct(lambda t: t.rstrip().endswith("?")),
        "exclamation_rate": pct(lambda t: "!" in t),
        "ellipsis_rate": pct(lambda t: "…" in t or "..." in t),
        "emoji": {
            "message_rate": pct(lambda t: any(is_emoji(c) for c in t)),
            "top": [e for e, _ in emoji.most_common(10)],
        },
        "contraction_rate": pct(
            lambda t: re.search(r"\b\w+['’](s|t|re|ve|ll|d|m)\b", t) is not None),
        "openers_top": [w for w, _ in openers.most_common(12)],
        "burst": {
            "multi_bubble_rate": round(
                sum(1 for b in bursts if b > 1) / len(bursts), 3),
            "median_burst": int(statistics.median(bursts)),
            "max_burst": max(bursts),
        },
    }


# Rule claims we can check mechanically against the card. Each: (substring
# that identifies the authored rule, lambda(card) -> (holds, evidence)).
RULE_CHECKS = [
    ("all lowercase", lambda c: (
        c["case"]["starts_lowercase"] >= 0.5,
        f"measured starts_lowercase={c['case']['starts_lowercase']:.0%}")),
    ("contraction", lambda c: (
        c["contraction_rate"] >= 0.3,
        f"measured contraction_rate={c['contraction_rate']:.0%}")),
    ("short", lambda c: (
        c["length"]["median"] <= 60,
        f"measured median_len={c['length']['median']}")),
]


def diff_rules(card, persona_path):
    try:
        p = json.load(open(os.path.expanduser(persona_path)))
    except (OSError, json.JSONDecodeError):
        return []
    rules = []
    def collect(v):
        if isinstance(v, str):
            rules.append(v)
        elif isinstance(v, list):
            for x in v:
                collect(x)
        elif isinstance(v, dict):
            for x in v.values():
                collect(x)
    collect(p)
    findings = []
    for needle, check in RULE_CHECKS:
        for r in rules:
            if needle in r.lower():
                holds, evidence = check(card)
                findings.append({
                    "rule": r[:100],
                    "verdict": "SUPPORTED" if holds else "CONTRADICTED",
                    "evidence": evidence,
                })
                break
    return findings


def selftest():
    con = sqlite3.connect(":memory:")
    con.executescript("""
        CREATE TABLE message (ROWID INTEGER PRIMARY KEY, handle_id INT,
            is_from_me INT, text TEXT, attributedBody BLOB, date INT,
            associated_message_type INT);
        CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, chat_identifier TEXT);
        CREATE TABLE chat_message_join (chat_id INT, message_id INT);
        INSERT INTO chat VALUES (1, 'iMessage;-;+15550000001');
    """)
    now_apple = (con.execute(
        "SELECT (strftime('%s','now') - strftime('%s','2001-01-01'))").fetchone()[0])
    msgs = [("hey", 0), ("on my way", 0), ("Loved “nice”", 2000), ("sounds good…", 0)]
    for i, (t, assoc) in enumerate(msgs):
        con.execute("INSERT INTO message VALUES (?,1,1,?,NULL,?,?)",
                    (i + 1, t, (now_apple - 100 + i * 10) * 1000000000, assoc))
        con.execute("INSERT INTO chat_message_join VALUES (1, ?)", (i + 1,))
    # attributedBody-only row: text NULL, blob carries the payload
    blob = b"junkNSStringjunk+\x0bhello there\x86tail"
    con.execute("INSERT INTO message VALUES (5,1,1,NULL,?,?,0)",
                (blob, (now_apple - 50) * 1000000000))
    con.execute("INSERT INTO chat_message_join VALUES (1, 5)")
    rows = con.execute(f"""
        SELECT {APPLE_EPOCH_SQL} AS ts, COALESCE(c.chat_identifier,'unknown'),
               m.text, m.attributedBody, COALESCE(m.associated_message_type,0)
        FROM message m
        LEFT JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        LEFT JOIN chat c ON c.ROWID = cmj.chat_id
        WHERE m.is_from_me = 1 ORDER BY ts""").fetchall()
    typed, reactions = [], 0
    for ts, ck, t, blob, a in rows:
        if t is None and blob is not None:
            t = extract_text_from_attributed_body(blob)
        if not t:
            continue
        if 2000 <= a <= 2005 or t.startswith(TAPBACK_PREFIXES):
            reactions += 1
            continue
        typed.append((ts, ck, t))
    assert len(typed) == 4 and reactions == 1, (len(typed), reactions)
    assert typed[-1][2] == "hello there", typed[-1]
    card = build_card(typed, reactions)
    assert card["reaction_rate"] == 0.2, card["reaction_rate"]
    assert card["terminal_punctuation"]["none"] > 0.5
    assert card["case"]["starts_lowercase"] > 0.5
    assert card["burst"]["multi_bubble_rate"] == 1.0  # all within 60s, same handle
    print("selftest OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="~/Library/Messages/chat.db")
    ap.add_argument("--days", type=int, default=180)
    ap.add_argument("--out", default="~/.human/personas/seth.style-card.json")
    ap.add_argument("--persona", default="~/.human/personas/seth.json")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return

    typed, reactions = fetch_messages(args.db, args.days)
    card = build_card(typed, reactions)
    card["rule_diff"] = diff_rules(card, args.persona)

    out = os.path.expanduser(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump(card, open(out, "w"), indent=2, ensure_ascii=False)
    print(json.dumps(card, indent=2, ensure_ascii=False))
    print(f"\nstyle card written: {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
