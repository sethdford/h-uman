#!/usr/bin/env python3
"""Specificity axis — the residual human tell (n=40 blind gate, 2026-07-27:
"AI goes generic where Seth names names").

Deterministic, LLM-free. Scores a reply by the specific tokens it carries:
  insider   — a name from the persona's contacts or an entity name from the
              knowledge graph (places, people, employers, things Seth has
              actually talked about)
  concrete  — numbers, times, money, dates, addresses
  proper    — a capitalized word that is not sentence-initial and not "I"
Reports per-100-char rates and the share of replies with >= 1 specific
token, for two corpora side by side:
  human   — Seth's own outbound texts (m3-corpus.jsonl, role=assistant)
  daemon  — the daemon's stored replies (memory.db messages, role=assistant)

Gate (docs/plans/2026-09-06-better-than-human item 3): daemon specificity
per reply >= human. Writes a JSON result under the plan's results/ dir when
--out is given. Read-only on every database it touches.

Usage: scripts/specificity_score.py [--daemon-last N] [--out FILE] [--show K]
"""
import argparse
import json
import os
import re
import sqlite3
import statistics
import sys
import time

HOME = os.path.expanduser("~")
M3 = os.path.join(HOME, ".human/training-data/m3-corpus.jsonl")
MEMORY_DB = os.path.join(HOME, ".human/memory.db")
GRAPH_DB = os.path.join(HOME, ".human/graph.db")
PERSONA = os.path.join(HOME, ".human/personas/seth.json")

CONCRETE = re.compile(
    r"(\$\s?\d[\d,]*(\.\d+)?|\b\d{1,2}(:\d{2})?\s?(am|pm)\b|\b\d{1,2}/\d{1,2}(/\d{2,4})?\b"
    r"|\b\d+(\.\d+)?\s?(k|%|min|mins|hrs?|hours?|days?|weeks?|months?|years?|miles?|lbs?|inches|ft)\b"
    r"|\b\d{3,}\b|\b\d+\b)",
    re.I,
)
STOP_INSIDER = {"user", "self", "home", "work", "away", "place", "apartment", "thing", "things",
                "stuff", "today", "tomorrow", "tonight", "week", "weekend", "monday", "tuesday",
                "wednesday", "thursday", "friday", "saturday", "sunday", "seth", "i", "me", "you"}


def readonly(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


def insider_vocab():
    vocab = set()
    try:
        p = json.load(open(PERSONA))
        for cid, c in (p.get("contacts") or {}).items():
            name = (c.get("name") or "").strip().lower()
            for part in name.replace("-", " ").split():
                if len(part) >= 3:
                    vocab.add(part)
    except Exception:
        pass
    try:
        db = readonly(GRAPH_DB)
        for (name,) in db.execute("SELECT name FROM entities"):
            n = (name or "").strip().lower()
            if not n or n in STOP_INSIDER or len(n) < 3:
                continue
            # multi-word entities count as a phrase; also index their rare words
            vocab.add(n)
            for part in n.split():
                if len(part) >= 5 and part not in STOP_INSIDER:
                    vocab.add(part)
    except Exception:
        pass
    return vocab - STOP_INSIDER


def score(text, vocab):
    t = text.strip()
    if not t:
        return None
    low = t.lower()
    words = re.findall(r"[a-z0-9']+", low)
    insider = 0
    seen = set()
    for w in words:
        if w in vocab and w not in seen:
            insider += 1
            seen.add(w)
    for phrase in vocab:
        if " " in phrase and phrase in low and phrase not in seen:
            insider += 1
            seen.add(phrase)
    concrete = len(CONCRETE.findall(t))
    proper = 0
    for sent in re.split(r"[.!?\n]+", t):
        toks = sent.strip().split()
        for tok in toks[1:]:
            core = tok.strip("\"'(),;:")
            if len(core) > 1 and core[0].isupper() and core != "I" and not core.isupper():
                proper += 1
    total = insider + concrete + proper
    return {"chars": len(t), "insider": insider, "concrete": concrete, "proper": proper,
            "total": total, "per100": 100.0 * total / max(len(t), 1), "any": total > 0}


def human_corpus():
    out = []
    if not os.path.exists(M3):
        return out
    for line in open(M3):
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("role") == "assistant" and d.get("content"):
            out.append(d["content"])
    return out


def daemon_corpus(last_n):
    db = readonly(MEMORY_DB)
    rows = db.execute("SELECT content FROM messages WHERE role='assistant' ORDER BY id DESC LIMIT ?",
                      (last_n,)).fetchall()
    return [r[0] for r in rows if r[0]]


def summarize(name, texts, vocab):
    scored = [s for s in (score(t, vocab) for t in texts) if s]
    if not scored:
        return {"corpus": name, "n": 0}
    return {
        "corpus": name,
        "n": len(scored),
        "mean_chars": round(statistics.mean(s["chars"] for s in scored), 1),
        "specific_per_reply": round(statistics.mean(s["total"] for s in scored), 3),
        "specific_per_100_chars": round(statistics.mean(s["per100"] for s in scored), 3),
        "share_with_any": round(sum(1 for s in scored if s["any"]) / len(scored), 3),
        "insider_per_reply": round(statistics.mean(s["insider"] for s in scored), 3),
        "concrete_per_reply": round(statistics.mean(s["concrete"] for s in scored), 3),
        "proper_per_reply": round(statistics.mean(s["proper"] for s in scored), 3),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daemon-last", type=int, default=300)
    ap.add_argument("--out")
    ap.add_argument("--show", type=int, default=0, help="print K lowest-scoring daemon replies")
    a = ap.parse_args()
    vocab = insider_vocab()
    human = human_corpus()
    daemon = daemon_corpus(a.daemon_last)
    res = {"measured_at": time.strftime("%Y-%m-%dT%H:%M:%S"), "insider_vocab_size": len(vocab),
           "human": summarize("human", human, vocab),
           "daemon": summarize("daemon", daemon, vocab)}
    h, d = res["human"], res["daemon"]
    if h.get("n") and d.get("n"):
        res["gate"] = {"metric": "specific_per_reply", "human": h["specific_per_reply"],
                       "daemon": d["specific_per_reply"],
                       "pass": d["specific_per_reply"] >= h["specific_per_reply"]}
    print(json.dumps(res, indent=2))
    if a.show and daemon:
        scored = sorted(((score(t, vocab) or {"total": 0})["total"], t) for t in daemon)
        print("\nlowest-scoring daemon replies:")
        for tot, t in scored[: a.show]:
            print(f"  [{tot}] {t[:110]!r}")
    if a.out:
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
        json.dump(res, open(a.out, "w"), indent=2)
    return 0 if res.get("gate", {}).get("pass") else 1


if __name__ == "__main__":
    sys.exit(main())
