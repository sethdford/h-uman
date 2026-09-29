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

LENGTH CONFOUND (2026-09-22). The original gate metric, specific-tokens
per REPLY, conflates two different defects with two different fixes:
  (1) density  — the reply goes generic per unit of text (the actual blind
                 rater tell: "AI goes generic where Seth names names")
  (2) length   — the reply simply says less
On 2026-09-20 the daemon measured 0.973 vs Seth 2.136 per reply, but its
replies averaged 38.7 chars against Seth's 71.3 — so roughly half the
headline gap was brevity, which the voice work installed ON PURPOSE
(ORPO deliberation suppression; v4-repair kept because terseness was
already solved). A gate that scores trained-in terseness as a specificity
defect points remediation at the wrong subsystem.

Per-100-char rates do NOT fix this: score() deduplicates `insider` within a
reply but not `concrete`/`proper`, so the metric mixes a saturating
component with two linear ones and per-100-char is biased AGAINST long
replies (Seth's density is understated). The unbiased normalizer is
LENGTH MATCHING — pair each daemon reply with a real Seth reply of
near-identical length and compare per-reply directly. Deterministic:
buckets are keyed by exact char length and drained in sorted order.

The headline gate is therefore the length-matched per-reply comparison,
reported with a bootstrap 95% CI on the delta so a point estimate is never
mistaken for a ranking (cf. the 44%-vs-55% proxy runs on one daemon).
Length parity is reported as a labelled DIAGNOSTIC, not a gate: short
replies passed the n=40 human gate at detection 0.225, so brevity is a
known-tolerated divergence, not a proven defect.

Writes a JSON result under the plan's results/ dir when --out is given.
Read-only on every database it touches.

Usage: scripts/specificity_score.py [--daemon-last N] [--out FILE] [--show K]
                                    [--max-delta CHARS] [--no-length-match]
"""
import argparse
import json
import os
import random
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
    # PROPER NOUNS ONLY (fixed 2026-09-22). graph.db stores topic PHRASES, not
    # named entities — 399 of 572 rows start lowercase ("a different
    # direction", "adventure", "all the things"). The previous version indexed
    # the whole phrase AND split out every word >= 5 chars, which injected
    # ordinary English into the "insider" class: 20 stoplist words made it into
    # the 1008-token vocab, and because common words are high-frequency they
    # DOMINATED the counts. Measured effect of the bug: the daemon's top
    # "insider entity" mentions were that(28), about(11), help(9), and
    # "insider mentions at parity" (daemon/Seth 0.94) was an artifact — with
    # the junk removed the real ratio is 0.60. A specificity metric has to
    # count names, so a token now earns a slot only if it is CAPITALIZED in
    # the source entity name (a real proper noun) or comes from a contact.
    try:
        db = readonly(GRAPH_DB)
        for (name,) in db.execute("SELECT name FROM entities"):
            raw = (name or "").strip()
            if not raw:
                continue
            words = raw.replace("-", " ").split()
            proper = [w for w in words if w[:1].isupper() and not w.isupper() or
                      (w.isupper() and len(w) >= 2)]
            if not proper:
                continue  # a wholly lowercase phrase is a topic, not a name
            # The full phrase counts only when it carries a proper noun.
            n = raw.lower()
            if len(n) >= 3 and n not in STOP_INSIDER:
                vocab.add(n)
            for w in proper:
                t = w.strip(".,;:'\"()").lower()
                if len(t) >= 3 and t not in STOP_INSIDER:
                    vocab.add(t)
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
        # Per-100-char densities are reported for decomposition only. `insider`
        # saturates (score() dedups it), so these are NOT gate-grade across
        # corpora of differing length — that is what length matching is for.
        "insider_per_100_chars": round(
            statistics.mean(100.0 * s["insider"] / max(s["chars"], 1) for s in scored), 3),
        "concrete_per_100_chars": round(
            statistics.mean(100.0 * s["concrete"] / max(s["chars"], 1) for s in scored), 3),
        "proper_per_100_chars": round(
            statistics.mean(100.0 * s["proper"] / max(s["chars"], 1) for s in scored), 3),
    }


def length_matched_human(human_texts, daemon_texts, max_delta=10):
    """Draw a human cohort whose reply lengths match the daemon's, 1:1.

    Buckets human replies by exact char length, then for each daemon reply
    (shortest first, so the scarce tail is served before the crowded middle)
    takes an unused human reply within +/- max_delta chars, searching outward
    from the exact length. Sampling is WITHOUT replacement, so the cohort is a
    real subsample of Seth rather than a reweighting. Returns
    (matched_texts, unmatched_daemon_count) — an honest population report.
    """
    by_len = {}
    for t in human_texts:
        s = (t or "").strip()
        if s:
            by_len.setdefault(len(s), []).append(s)
    for bucket in by_len.values():
        bucket.sort()  # determinism within a length
    matched, unmatched = [], 0
    for t in sorted(((x or "").strip() for x in daemon_texts if (x or "").strip()), key=len):
        target, picked = len(t), None
        for radius in range(max_delta + 1):
            for length in ((target - radius, target + radius) if radius else (target,)):
                bucket = by_len.get(length)
                if bucket:
                    picked = bucket.pop(0)
                    if not bucket:
                        by_len.pop(length, None)
                    break
            if picked is not None:
                break
        if picked is None:
            unmatched += 1
        else:
            matched.append(picked)
    return matched, unmatched


def bootstrap_delta_ci(daemon_totals, human_totals, iters=2000, seed=0):
    """95% CI on mean(daemon) - mean(human) by paired-resample bootstrap."""
    if not daemon_totals or not human_totals:
        return None
    rng = random.Random(seed)
    n = min(len(daemon_totals), len(human_totals))
    deltas = []
    for _ in range(iters):
        d = sum(daemon_totals[rng.randrange(len(daemon_totals))] for _ in range(n)) / n
        h = sum(human_totals[rng.randrange(len(human_totals))] for _ in range(n)) / n
        deltas.append(d - h)
    deltas.sort()
    return [round(deltas[int(0.025 * iters)], 3), round(deltas[int(0.975 * iters)], 3)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daemon-last", type=int, default=300)
    ap.add_argument("--out")
    ap.add_argument("--show", type=int, default=0, help="print K lowest-scoring daemon replies")
    ap.add_argument("--max-delta", type=int, default=10,
                    help="max char-length difference when length-matching the human cohort")
    ap.add_argument("--no-length-match", action="store_true",
                    help="gate on the raw (length-confounded) per-reply comparison instead")
    a = ap.parse_args()
    vocab = insider_vocab()
    human = human_corpus()
    daemon = daemon_corpus(a.daemon_last)
    res = {"measured_at": time.strftime("%Y-%m-%dT%H:%M:%S"), "insider_vocab_size": len(vocab),
           "human": summarize("human", human, vocab),
           "daemon": summarize("daemon", daemon, vocab)}
    h, d = res["human"], res["daemon"]
    if h.get("n") and d.get("n"):
        # Raw per-reply comparison, kept for continuity with the 2026-09-20
        # reading — explicitly labelled as confounded by reply length.
        raw = {"metric": "specific_per_reply", "human": h["specific_per_reply"],
               "daemon": d["specific_per_reply"],
               "pass": d["specific_per_reply"] >= h["specific_per_reply"],
               "confounded_by": "reply length (see module docstring)"}
        parity = round(d["mean_chars"] / max(h["mean_chars"], 0.1), 3)
        res["diagnostics"] = {
            "raw_per_reply_gate": raw,
            "length_parity": {"human_mean_chars": h["mean_chars"],
                              "daemon_mean_chars": d["mean_chars"], "ratio": parity,
                              "note": "DIAGNOSTIC, not a gate: short replies passed the "
                                      "n=40 human gate at detection 0.225, so brevity is a "
                                      "known-tolerated divergence, not a proven defect."},
        }
        if a.no_length_match:
            res["gate"] = raw
        else:
            matched, unmatched = length_matched_human(human, daemon, a.max_delta)
            m = summarize("human_length_matched", matched, vocab)
            res["human_length_matched"] = m
            if not m.get("n"):
                res["gate"] = dict(raw, note="length matching produced no cohort; fell back")
            else:
                d_tot = [s["total"] for s in (score(t, vocab) for t in daemon) if s]
                m_tot = [s["total"] for s in (score(t, vocab) for t in matched) if s]
                delta = round(d["specific_per_reply"] - m["specific_per_reply"], 3)
                ci = bootstrap_delta_ci(d_tot, m_tot)
                res["gate"] = {
                    "metric": "specific_per_reply (length-matched human cohort)",
                    "human_length_matched": m["specific_per_reply"],
                    "daemon": d["specific_per_reply"],
                    "delta": delta,
                    "delta_ci95": ci,
                    "matched_n": m["n"],
                    "unmatched_daemon_replies": unmatched,
                    "max_delta_chars": a.max_delta,
                    "human_matched_mean_chars": m["mean_chars"],
                    "pass": d["specific_per_reply"] >= m["specific_per_reply"],
                    "significant": bool(ci and (ci[0] > 0 or ci[1] < 0)),
                }
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
