#!/usr/bin/env python3
"""Insight overuse — twin vs Seth.

The daemon logs one line per reply under HU_INSIGHT_OVERUSE=shadow
(src/daemon/daemon_insight_overuse.c):

    [insight-overuse] shadow: injected=N surfaced=S prompted=P unprompted=U for <contact>

This script aggregates those lines and, with --seth-baseline, computes the
SAME counts over Seth's own replies in chat.db against the SAME insight block
each contact gets today (memory.db, same query and budget as the loader). The
target is not "less memory" but "as much unprompted memory as Seth brings up":
PAS (arXiv 2609.04676) says models overuse persona attributes regardless of
context; the counter-metric is the gap to the real person.

The tokenizer below mirrors the C one exactly (>=4 chars, must contain a
letter, stop list, whole-word case-insensitive match). tests/test_insight_overuse_report.py
pins the parity on the C test's fixture.

Advisory: prints a table, writes JSON with --json, never writes a gate.
Anachronism to keep in mind: the baseline scores PAST Seth replies against the
CURRENT insight block, so it measures how often Seth's texts carry these
memory tokens, not what he knew at the time.
"""
import argparse
import json
import os
import re
import sqlite3
import sys
import time

HOME = os.path.expanduser("~")
LOG = os.path.join(HOME, ".human/logs/service-loop-error.log")
MEMORY_DB = os.path.join(HOME, ".human/memory.db")
CHAT_DB = os.path.join(HOME, "Library/Messages/chat.db")

# Must match src/agent/memory_loader.h and src/daemon/daemon_insight_overuse.c.
MAX_ITEMS, MAX_BYTES, MIN_CONFIDENCE = 8, 900, 0.5
STOP = {
    "with", "that", "this", "have", "they", "them", "their", "about", "just",
    "like", "what", "when", "from", "will", "your", "been", "were", "also",
    "into", "some", "than", "then", "there", "would", "could", "should", "really",
    "think", "going", "want", "know", "dont", "doesnt", "didnt", "still", "because",
    "thing", "things", "week", "today", "tomorrow", "tonight", "yeah", "okay",
}
LINE_RE = re.compile(
    r"insight-overuse\].*?injected=(\d+) surfaced=(\d+) prompted=(\d+) unprompted=(\d+) for (\S+)")
TOKEN_RE = re.compile(r"[A-Za-z0-9']+")


def tokens(text):
    """Deduplicated content tokens in first-seen order (the C dedupe)."""
    out, seen = [], set()
    for run in TOKEN_RE.findall(text or ""):
        if len(run) < 4 or not any(c.isalpha() for c in run):
            continue
        t = run.lower()
        if t in STOP or t in seen:
            continue
        seen.add(t)
        out.append(t)
    return out


def has_word(hay, tok):
    """Whole-word, case-insensitive; boundaries are non-alphanumerics, exactly
    like hu_str_contains_word_ci_n (an apostrophe IS a boundary: "mindy's"
    contains the word "mindy")."""
    return re.search(r"(?<![A-Za-z0-9])" + re.escape(tok) + r"(?![A-Za-z0-9])",
                     hay or "", re.IGNORECASE) is not None


def count(insights, inbound, reply):
    """(injected, surfaced, prompted) — mirrors hu_insight_overuse_count."""
    injected = surfaced = prompted = 0
    for t in tokens(insights):
        injected += 1
        if has_word(reply, t):
            surfaced += 1
            if has_word(inbound, t):
                prompted += 1
    return injected, surfaced, prompted


def parse_log(lines):
    rows = []
    for ln in lines:
        m = LINE_RE.search(ln)
        if m:
            inj, sur, pro, unp, cid = m.groups()
            rows.append({"contact": cid, "injected": int(inj), "surfaced": int(sur),
                         "prompted": int(pro), "unprompted": int(unp)})
    return rows


def aggregate(rows):
    n = len(rows)
    if n == 0:
        return {"n": 0}
    with_block = [r for r in rows if r["injected"] > 0]
    unp = [r["surfaced"] - r["prompted"] for r in rows]
    return {
        "n": n,
        "n_with_block": len(with_block),
        "unprompted_per_reply": round(sum(unp) / n, 3),
        "unprompted_rate": round(sum(r["surfaced"] - r["prompted"] for r in with_block)
                                 / max(sum(r["injected"] for r in with_block), 1), 4),
        "share_replies_with_unprompted": round(sum(1 for u in unp if u > 0) / n, 3),
        "prompted_per_reply": round(sum(r["prompted"] for r in rows) / n, 3),
    }


def render_block(mem, contact_id):
    """Same query, order, and byte budget as contact_insights_repo_sqlite.c."""
    rows = mem.execute(
        "SELECT insight, as_of_ms FROM contact_insights WHERE contact_id=? AND retired_at_ms=0 "
        "AND confidence>=? ORDER BY as_of_ms DESC, id DESC LIMIT ?",
        (contact_id, MIN_CONFIDENCE, MAX_ITEMS)).fetchall()
    out = ""
    for insight, as_of in rows:
        mon = time.strftime("%b %Y", time.gmtime(as_of / 1000)) if as_of else "?"
        line = f"- {insight} (as of {mon})\n"
        if len(out.encode()) + len(line.encode()) > MAX_BYTES:
            break
        out += line
    return out


def seth_baseline(since_days, min_len=8):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
    from export_seth_triples import decode_attributed_body  # noqa: E402

    mem = sqlite3.connect(f"file:{MEMORY_DB}?mode=ro", uri=True)
    contacts = [r[0] for r in mem.execute(
        "SELECT DISTINCT contact_id FROM contact_insights WHERE retired_at_ms=0")]
    blocks = {c: render_block(mem, c) for c in contacts}
    blocks = {c: b for c, b in blocks.items() if b}
    if not blocks:
        return {"n": 0, "error": "no contact has a live insight block"}
    chat = sqlite3.connect(f"file:{CHAT_DB}?mode=ro", uri=True)
    # Apple epoch, nanoseconds since 2001-01-01.
    cutoff = int((time.time() - since_days * 86400 - 978307200) * 1e9)
    rows = chat.execute(
        "SELECT h.id, m.is_from_me, m.text, m.attributedBody FROM message m "
        "LEFT JOIN handle h ON h.ROWID = m.handle_id "
        "WHERE m.associated_message_type = 0 AND m.item_type = 0 AND m.date > ? "
        "ORDER BY h.id, m.date ASC", (cutoff,)).fetchall()
    last_inbound, out = {}, []
    for handle, is_from_me, text, body in rows:
        if handle not in blocks:
            continue
        content = (text or "").strip() or (decode_attributed_body(body) or "").strip()
        if not content:
            continue
        if is_from_me == 0:
            last_inbound[handle] = content
            continue
        if len(content) < min_len:
            continue
        inj, sur, pro = count(blocks[handle], last_inbound.get(handle, ""), content)
        out.append({"contact": handle, "injected": inj, "surfaced": sur, "prompted": pro,
                    "unprompted": sur - pro})
    agg = aggregate(out)
    agg["contacts"] = len(blocks)
    agg["since_days"] = since_days
    return agg


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--log", default=LOG)
    ap.add_argument("--seth-baseline", action="store_true",
                    help="score Seth's own chat.db replies against today's insight blocks")
    ap.add_argument("--since-days", type=int, default=60)
    ap.add_argument("--json")
    a = ap.parse_args()
    try:
        with open(a.log, errors="replace") as f:
            twin = aggregate(parse_log(f))
    except FileNotFoundError:
        twin = {"n": 0, "error": f"no log at {a.log}"}
    report = {"twin": twin}
    if a.seth_baseline:
        report["seth"] = seth_baseline(a.since_days)
    for who in ("twin", "seth"):
        r = report.get(who)
        if not r:
            continue
        if r.get("n", 0) == 0:
            print(f"{who}: no replies measured ({r.get('error', 'HU_INSIGHT_OVERUSE not shadow?')})")
            continue
        print(f"{who}: n={r['n']} with_block={r['n_with_block']} "
              f"unprompted/reply={r['unprompted_per_reply']} rate={r['unprompted_rate']} "
              f"share>=1={r['share_replies_with_unprompted']} prompted/reply={r['prompted_per_reply']}")
    t, s = report.get("twin", {}), report.get("seth", {})
    if t.get("n") and s.get("n"):
        d = t["unprompted_rate"] - s["unprompted_rate"]
        print(f"gap: twin unprompted rate {t['unprompted_rate']} vs seth {s['unprompted_rate']} "
              f"({'+' if d >= 0 else ''}{d:.4f}); target |gap| <= 0.05")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(report, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
