#!/usr/bin/env python3
"""Offline A/B for the insight stream (item 3 of the better-than-human plan).

Question: does the "What you actually remember about them" block raise reply
specificity? The feature-gate rule says LIVE needs a measurement, and the
metric is scripts/specificity_score.py's specific-tokens-per-reply.

Design: for each contact with live insights, take its most recent inbound
messages from the daemon's stored turns, and generate a reply twice with the
LOCAL model — same compiled persona prompt (`human persona show seth`), same
message, same sampling — once with the contact's insight block appended
exactly as the memory loader renders it, once without. Score both reply sets.
Paired by message, so noise cancels; report the mean delta and how many
pairs improved.

Read-only on every database. Local model only.

Usage: scripts/insight_ab.py [--per-contact 4] [--out FILE] [--human-cli PATH]
"""
import argparse
import json
import os
import sqlite3
import statistics
import subprocess
import sys
import time
import urllib.request
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from specificity_score import insider_vocab, score  # noqa: E402

HOME = os.path.expanduser("~")
MEMORY_DB = os.path.join(HOME, ".human/memory.db")
URL = "http://127.0.0.1:8741/v1/chat/completions"
MODEL = "GLM-4.5-Air-4bit"
HEADER = "### What you actually remember about them (weave in naturally, never recite):\n"


def readonly(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


def persona_prompt(human_cli):
    out = subprocess.run([human_cli, "persona", "show", "seth"], capture_output=True, text=True,
                         timeout=60)
    text = out.stdout.strip()
    if not text:
        raise SystemExit("persona show returned nothing")
    return text


def month(ms):
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%b %Y")


def insight_block(db, contact):
    rows = db.execute(
        "SELECT insight, as_of_ms FROM contact_insights WHERE contact_id=? AND retired_at_ms=0 "
        "AND confidence>=0.5 ORDER BY as_of_ms DESC, id DESC LIMIT 8", (contact,)).fetchall()
    lines, total = [], 0
    for ins, as_of in rows:
        line = f"- {ins} (as of {month(as_of)})\n" if as_of else f"- {ins}\n"
        if total + len(line) > 900:
            break
        lines.append(line)
        total += len(line)
    return HEADER + "".join(lines) if lines else ""


def recent_inbound(db, contact, n):
    rows = db.execute(
        "SELECT id, content FROM messages WHERE session_id=? AND role='user' ORDER BY id DESC "
        "LIMIT ?", (contact, n * 3)).fetchall()
    out = []
    for _id, c in rows:
        c = (c or "").strip()
        if 8 <= len(c) <= 300 and not c.startswith(("Liked", "Loved", "Laughed", "Emphasized")):
            out.append(c)
        if len(out) >= n:
            break
    return out


STOP = set("about after again their there these those which while would could should".split())


def grounded(reply, block):
    """Does the reply draw on the contact's own thread? Share of the block's
    content words (>=5 chars, not stopwords) that appear in the reply; a reply
    is 'grounded' when it reuses at least two. Specificity rewards any name;
    this rewards THEIR names — the difference between name-dropping and
    remembering."""
    import re
    words = {w for w in re.findall(r"[a-z][a-z']{4,}", block.lower()) if w not in STOP}
    hits = {w for w in re.findall(r"[a-z][a-z']{4,}", reply.lower()) if w in words}
    return len(hits)


def generate(system, user):
    req = {"model": MODEL, "max_tokens": 80, "temperature": 0.7, "seed": 7,
           "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
           "chat_template_kwargs": {"enable_thinking": False}}
    d = json.load(urllib.request.urlopen(urllib.request.Request(
        URL, data=json.dumps(req).encode(), headers={"Content-Type": "application/json"}),
        timeout=300))
    return (d["choices"][0]["message"].get("content") or "").strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-contact", type=int, default=4)
    ap.add_argument("--out")
    ap.add_argument("--human-cli", default=os.path.join(HOME, ".local/bin/human"))
    a = ap.parse_args()
    vocab = insider_vocab()
    head = persona_prompt(a.human_cli)
    db = readonly(MEMORY_DB)
    contacts = [r[0] for r in db.execute(
        "SELECT DISTINCT contact_id FROM contact_insights WHERE retired_at_ms=0")]
    pairs = []
    t0 = time.time()
    for contact in contacts:
        block = insight_block(db, contact)
        if not block:
            continue
        for msg in recent_inbound(db, contact, a.per_contact):
            base = f"{head}\n\nYou are texting with {contact}. Reply as Seth would, one text.\n"
            r_off = generate(base, msg)
            r_on = generate(base + "\n" + block, msg)
            s_off, s_on = score(r_off, vocab), score(r_on, vocab)
            if not s_off or not s_on:
                continue
            pairs.append({"contact": contact, "msg": msg, "off": r_off, "on": r_on,
                          "off_score": s_off["total"], "on_score": s_on["total"],
                          "off_grounded": grounded(r_off, block),
                          "on_grounded": grounded(r_on, block)})
            print(f"[{s_off['total']}->{s_on['total']}] {msg[:50]!r}\n"
                  f"     off: {r_off[:90]!r}\n     on : {r_on[:90]!r}")
    if not pairs:
        print("no pairs")
        return 2
    off = [p["off_score"] for p in pairs]
    on = [p["on_score"] for p in pairs]
    improved = sum(1 for p in pairs if p["on_score"] > p["off_score"])
    worse = sum(1 for p in pairs if p["on_score"] < p["off_score"])
    res = {"measured_at": time.strftime("%Y-%m-%dT%H:%M:%S"), "n_pairs": len(pairs),
           "contacts": len(contacts), "seconds": round(time.time() - t0, 1),
           "off_mean": round(statistics.mean(off), 3), "on_mean": round(statistics.mean(on), 3),
           "delta": round(statistics.mean(on) - statistics.mean(off), 3),
           "improved": improved, "worse": worse, "tied": len(pairs) - improved - worse,
           "off_share_any": round(sum(1 for x in off if x > 0) / len(off), 3),
           "on_share_any": round(sum(1 for x in on if x > 0) / len(on), 3),
           "off_grounded_share": round(sum(1 for p in pairs if p["off_grounded"] >= 2) / len(pairs), 3),
           "on_grounded_share": round(sum(1 for p in pairs if p["on_grounded"] >= 2) / len(pairs), 3),
           "human_reference": 2.134, "pairs": pairs}
    summary = {k: v for k, v in res.items() if k != "pairs"}
    print(json.dumps(summary, indent=2))
    if a.out:
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
        json.dump(res, open(a.out, "w"), indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
