#!/usr/bin/env python3
"""reply_pairs.py — (inbound, the user's next reply) pairs from chat.db.

The measured cards (style, emotion) describe the user's texts one message at
a time. Two humanness gaps found in 2026-09 are about how the user answers a
KIND of inbound: a sad/frustrated text (emotion card `distress_reply`) and a
long or question-bearing text (style card `substantive_reply`). Both need the
same read: inbound rows matching a predicate → the user's next in-chat reply
within a window. One reader here so the two axes cannot drift apart.

Read-only + immutable chat.db; attributedBody decoded by
eval_persona_evolution.decode_attributed_body; no text leaves the process.
"""
import os
import re
import sqlite3
import statistics

DEFAULT_CHATDB = os.path.expanduser("~/Library/Messages/chat.db")
REPLY_WINDOW_S = 30 * 60

DISTRESS_MARKERS = re.compile(
    r"(😓|😢|😭|😞|😔|💔|🥺|\bugh\b|\bsad\b|\bstressed\b|\bfrustrat|\bsucks\b|\bworst\b|"
    r"\bcrying\b|\btired\b|\bhard day\b|\bbad day\b|\bexhausted\b|\bmiss you\b|\bhate\b)",
    re.I,
)
_SMALL_TALK_QUESTION = re.compile(r"\b(how are you|hru|wyd|what are you doing|what'?s up)\b", re.I)


def is_distress(text: str) -> bool:
    return bool(DISTRESS_MARKERS.search(text))


def is_substantive(text: str) -> bool:
    """Long, or a real question (not small talk). The kind of inbound the
    multi-turn harness's debate / news / advice scenarios are made of."""
    if len(text) >= 150:
        return True
    return "?" in text and len(text) >= 60 and not _SMALL_TALK_QUESTION.search(text)


def fetch_reply_pairs(db_path: str, days: int, predicate, window_s: int = REPLY_WINDOW_S):
    """[(inbound_text, user_reply)] for inbounds where predicate(text) holds,
    paired with the user's next reply in the same chat within window_s.
    Attachment-only replies (U+FFFC) and bare links are skipped."""
    from eval_persona_evolution import decode_attributed_body  # local, stdlib

    con = sqlite3.connect(f"file:{db_path}?mode=ro&immutable=1", uri=True)
    try:
        rows = con.execute(
            """
            SELECT m.date, m.text, m.attributedBody, m.is_from_me, c.chat_id
            FROM message m JOIN chat_message_join c ON c.message_id = m.ROWID
            WHERE (m.text IS NOT NULL OR m.attributedBody IS NOT NULL)
              AND COALESCE(m.associated_message_type, 0) = 0
              AND m.date > (strftime('%s','now') - ? * 86400 - 978307200) * 1000000000
            ORDER BY c.chat_id, m.date
            """,
            (int(days),),
        ).fetchall()
    finally:
        con.close()

    def text_of(t, blob):
        if t and t.strip():
            return t.strip()
        return (decode_attributed_body(blob) or "").strip() if blob is not None else ""

    by_chat = {}
    for date, t, blob, me, chat in rows:
        by_chat.setdefault(chat, []).append((date, t, blob, me))
    pairs = []
    for msgs in by_chat.values():
        for i, (date, t, blob, me) in enumerate(msgs):
            if me:
                continue
            inbound = text_of(t, blob)
            if not inbound or not predicate(inbound):
                continue
            for date2, t2, blob2, me2 in msgs[i + 1:i + 6]:
                if not me2:
                    continue
                if date2 - date > window_s * 10**9:
                    break
                reply = text_of(t2, blob2)
                if reply and reply != "￼" and not reply.startswith("http"):
                    pairs.append((inbound, reply))
                break
    return pairs


_SENTENCE_END = re.compile(r"[.!?]+(\s|$)")
_ANSWER_FIRST = re.compile(r"^(yes|yeah|yep|yup|no|nah|nope|sure|idk|ok|okay|maybe|probably)\b", re.I)
# Reflexive agreement as the FIRST word (an optional "lol"/"haha"/"ok" before
# it): the shape the multi-turn judge calls "yes-man". Bare yes/no/ok/sure
# are answers to a question, not agreement, and are not counted here.
_AGREE_OPENER = re.compile(
    r"^(?:(?:lol|haha|hahaha|ok|okay)\s+)?"
    r"(yeah|yea|yep|yup|true|exactly|totally|100%|fr|for sure|fair|agreed|same|right|def|definitely)"
    r"(?![A-Za-z0-9'])",  # not \b: "100%." has no word boundary after the %
    re.I,
)


def is_agreement_opener(reply: str) -> bool:
    return bool(_AGREE_OPENER.match(reply.strip()))


def agreement_opener_rate(replies) -> float:
    """Share of replies that open on reflexive agreement. Judge-free.
    Measured 2026-09-13: the persona's substantive replies 0.06 (n=65); the
    twin's last-third replies in the substantive scenarios 0.46-0.62 (four
    3-repeat runs, off and live arms alike)."""
    replies = list(replies)
    if not replies:
        return 0.0
    return sum(1 for r in replies if is_agreement_opener(r)) / len(replies)


def reply_stats(pairs) -> dict:
    """Judge-free shape of the replies: n, median chars, share <= 60 chars,
    median sentences, answer-first share, reply/inbound length ratio."""
    n = len(pairs)
    if n == 0:
        return {"n": 0}
    lengths = sorted(len(r) for _, r in pairs)
    sentences = [max(1, len(_SENTENCE_END.findall(r))) for _, r in pairs]
    return {
        "n": n,
        "median_chars": lengths[n // 2],
        "share_le_60_chars": sum(1 for l in lengths if l <= 60) / n,
        "median_sentences": statistics.median(sentences),
        "answer_first_rate": sum(1 for _, r in pairs if _ANSWER_FIRST.match(r)) / n,
        "agreement_opener_rate": agreement_opener_rate(r for _, r in pairs),
        "median_reply_to_inbound_ratio": statistics.median(len(r) / max(1, len(i)) for i, r in pairs),
        "window_seconds": REPLY_WINDOW_S,
    }
