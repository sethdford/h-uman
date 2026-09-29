#!/usr/bin/env python3
"""Mine DPO pairs from confidently-machine-typical generations (Binoculars).

Sibling of judge_to_dpo.py, mining a DIFFERENT signal. judge_to_dpo mines the
rounds a cloud judge could *see through*; this mines the rounds that are
statistically typical of the generator's own distribution — including replies
that FOOLED the judge. On the 07-24 corpus those two sets overlap in only 9 of
28 trials; 6 trials were binoculars-only. Those 6 are this miner's reason to
exist.

    chosen   = the real Seth reply  (human ground truth from the trial)
    rejected = h-uman's generation, selected because score < threshold
    prompt   = the incoming message

Why this does NOT need judge_to_dpo's ALLOWED_JUDGE_FAMILIES gate
----------------------------------------------------------------
That gate exists because a same-family judge's *verdict* becomes the training
label — and a judge blind to its own family's tells trains the model to fool a
judge that already cannot see it. Here no verdict is ever a label: the
preference label is real Seth text, ground truth from chat.db. The Binoculars
score is only a SELECTOR deciding which rejected samples are worth training
against. A bad score can pick a suboptimal subset; it cannot invent a bad
`chosen`.

The real risk is different and worth naming: the score is measured against the
generator's *current* distribution, so the target moves after every training
round. Iterated without recalibration, "move off your own typical set" can
drift toward merely-atypical (rare tokens, degenerate text) rather than
Seth-like. Three things hold that in check — (1) `chosen` is always real human
text, anchoring the objective; (2) thresholds MUST be recalibrated per adapter
(docs/research/2026-07-25-binoculars-discriminator.md); (3) the per-run cap
keeps these a small minority of the corpus.

Safety posture (OFF -> SHADOW -> LIVE, per .claude/rules/feature-gate-requires-measurement.md)
    default        SHADOW: report + optional JSONL, writes NOTHING to the DB
    --live         insert into dpo_pairs (source='binoculars_miner')
Every consumer of dpo_pairs reads it UNFILTERED (finetune-gemma.py,
src/ml/dpo.c), and src/ml/dpo.c evicts FIFO by lowest id once past max_pairs —
so an over-eager miner would delete older human-verified pairs. Hence --live is
opt-in and --max-insert is enforced.

Usage:
    # shadow: what would it mine?
    python3 binoculars_to_dpo.py --pairs ../../data/eval_blinded_ab.json

    # shadow + record candidates for review
    python3 binoculars_to_dpo.py --pairs data/eval_blinded_ab.json \
        --shadow-out ~/.human/logs/eval-archive/binoc-dpo-candidates.jsonl

    # live insert (requires a recalibrated threshold for the current adapter)
    python3 binoculars_to_dpo.py --pairs data/eval_blinded_ab.json --live

    python3 binoculars_to_dpo.py --selftest
"""
import argparse
import json
import os
import re
import sqlite3
import sys
import time

SOURCE = "binoculars_miner"
DB_DEFAULT = os.path.expanduser("~/.human/memory.db")
# Calibrated 2026-07-25 vs seth-lora-v5-8bit-20260718 (5% FPR on 100 held-out
# real messages). RECALIBRATE after any adapter promotion.
DEFAULT_THRESHOLD = float(os.environ.get("HU_BINOCULARS_THR_FPR5", "0.9643"))
# Score at/below which a generation is "maximally machine" for margin scaling;
# observed minimum on the calibration corpus was 0.587.
DEFAULT_FLOOR = 0.60
# Binoculars on very short texts is noise (per-message AUC is carried by
# longer replies) — do not mine pairs whose rejected side is below this.
DEFAULT_MIN_TOKENS = 4
DEFAULT_MAX_INSERT = 25

DPO_PAIRS_DDL = (
    "CREATE TABLE IF NOT EXISTS dpo_pairs("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "prompt TEXT, chosen TEXT, rejected TEXT, "
    "margin REAL, timestamp INTEGER, source TEXT)"
)


# Stray leading byte from the attributedBody decode: ',I don't know…',
# '0Oh hey there!…', '%A ton…', 'HYeah works for me…'. 131/644 (20%) of
# ground_truth.jsonl carries this as of 2026-07-25. A corrupted `chosen`
# would teach the model to EMIT the garbage prefix, so it is refused.
# Deliberately conservative: a false positive costs one skipped pair, a false
# negative poisons the training corpus.
_CORRUPT_PREFIX = re.compile(r"^[^\w\s]\w|^\d[A-Z]|^[A-Z][A-Z][a-z]")


def looks_corrupted(text):
    """True if text carries the known leading-byte extraction artifact."""
    return bool(_CORRUPT_PREFIX.match(text or ""))


def margin_for(score, threshold, floor):
    """Normalized depth below the flag threshold -> margin in [0, 1].

    score == threshold -> 0.0 (borderline);  score <= floor -> 1.0 (maximally
    machine-typical). Monotonic and clamped in between.
    """
    span = threshold - floor
    if span <= 0:
        return 1.0
    return max(0.0, min(1.0, (threshold - score) / span))


def select_candidates(doc, threshold, min_tokens, require_judge_detected,
                      allow_corrupt=False):
    """Pick trials whose AI reply is confidently machine-typical.

    Returns (candidates, stats). Reads the per-trial scores that
    eval_blinded_ab.py --binoculars merged into the results file.
    """
    binoc = doc.get("binoculars") or {}
    by_trial = binoc.get("ai_by_trial") or {}
    stats = {"trials": 0, "no_score": 0, "above_threshold": 0,
             "too_short": 0, "judge_fooled_skipped": 0, "blank": 0,
             "corrupt_chosen": 0, "selected": 0}
    if not by_trial:
        return [], stats

    out = []
    for t in doc.get("trials") or []:
        stats["trials"] += 1
        entry = by_trial.get(str(t.get("i")))
        if not entry:
            stats["no_score"] += 1
            continue
        if entry["score"] >= threshold:
            stats["above_threshold"] += 1
            continue
        if entry["n_tokens"] < min_tokens:
            stats["too_short"] += 1
            continue
        # AND-mode: also require the cloud judge to have caught it. Higher
        # precision, but discards this miner's unique contribution (the
        # judge-fooled-but-machine-typical rows).
        if require_judge_detected and not t.get("judge_correct"):
            stats["judge_fooled_skipped"] += 1
            continue
        prompt = (t.get("incoming") or "").strip()
        chosen = (t.get("real_seth") or "").strip()
        rejected = (t.get("ai_response") or "").strip()
        if not chosen or not rejected:
            stats["blank"] += 1
            continue
        if not allow_corrupt and looks_corrupted(chosen):
            stats["corrupt_chosen"] += 1
            continue
        out.append({"trial": t.get("i"), "prompt": prompt, "chosen": chosen,
                    "rejected": rejected, "score": entry["score"],
                    "n_tokens": entry["n_tokens"],
                    "judge_detected": bool(t.get("judge_correct"))})
    # Most machine-typical first, so --max-insert keeps the strongest signal.
    out.sort(key=lambda c: c["score"])
    stats["selected"] = len(out)
    return out, stats


def already_present(con, prompt, chosen, rejected):
    """Content dedup across ALL sources.

    Deliberately not scoped to source=binoculars_miner: judge_to_dpo can mine
    the same correction from the same trial, and inserting it twice would
    double-weight one example in an unfiltered training corpus.
    """
    cur = con.execute(
        "SELECT source FROM dpo_pairs WHERE prompt=? AND chosen=? AND rejected=? LIMIT 1",
        (prompt, chosen, rejected))
    row = cur.fetchone()
    return row[0] if row else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pairs", help="eval_blinded_ab.json with a merged "
                    "'binoculars' key (written by eval_blinded_ab.py --binoculars)")
    ap.add_argument("--db", default=DB_DEFAULT)
    ap.add_argument("--live", action="store_true",
                    help="actually INSERT into dpo_pairs (default: shadow, no writes)")
    ap.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD,
                    help=f"flag below this score (default {DEFAULT_THRESHOLD}, "
                         "adapter-specific — recalibrate on promotion)")
    ap.add_argument("--floor", type=float, default=DEFAULT_FLOOR,
                    help="score mapped to margin 1.0")
    ap.add_argument("--min-tokens", type=int, default=DEFAULT_MIN_TOKENS,
                    help="skip rejected texts shorter than this (score is noise)")
    ap.add_argument("--max-insert", type=int, default=DEFAULT_MAX_INSERT,
                    help="hard cap on rows inserted per run (FIFO-eviction guard)")
    ap.add_argument("--require-judge-detected", action="store_true",
                    help="AND-mode: only mine trials the cloud judge ALSO caught "
                         "(higher precision, drops this miner's unique rows)")
    ap.add_argument("--shadow-out", help="append candidate pairs as JSONL here "
                    "(works in shadow mode; no DB writes)")
    ap.add_argument("--allow-corrupt-chosen", action="store_true",
                    help="do NOT use: disables the leading-byte guard on real "
                         "Seth text (would train the model to emit the artifact)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        selftest()
        return
    if not a.pairs:
        ap.error("--pairs is required (or --selftest)")

    with open(a.pairs) as f:
        doc = json.load(f)
    if not (doc.get("binoculars") or {}).get("ai_by_trial"):
        print("BINOCULARS_TO_DPO_SKIP reason=no-per-trial-scores "
              "(run eval_blinded_ab.py --binoculars first)", file=sys.stderr)
        sys.exit(0)

    cands, stats = select_candidates(doc, a.threshold, a.min_tokens,
                                     a.require_judge_detected,
                                     allow_corrupt=a.allow_corrupt_chosen)
    capped = cands[:a.max_insert]
    n_over_cap = len(cands) - len(capped)

    if a.shadow_out:
        os.makedirs(os.path.dirname(os.path.abspath(a.shadow_out)), exist_ok=True)
        stamp = int(time.time())
        with open(a.shadow_out, "a") as f:
            for c in capped:
                f.write(json.dumps({**c, "ts": stamp, "source": SOURCE,
                                    "margin": round(margin_for(c["score"], a.threshold,
                                                               a.floor), 4)}) + "\n")

    inserted = skipped_dup = 0
    dup_sources = {}
    if a.live and capped:
        con = sqlite3.connect(a.db)
        con.execute(DPO_PAIRS_DDL)
        now = int(time.time())  # SECONDS — matches every source except 'arena'
        for c in capped:
            dup = already_present(con, c["prompt"], c["chosen"], c["rejected"])
            if dup:
                skipped_dup += 1
                dup_sources[dup] = dup_sources.get(dup, 0) + 1
                continue
            con.execute(
                "INSERT INTO dpo_pairs(prompt, chosen, rejected, margin, timestamp, source) "
                "VALUES(?,?,?,?,?,?)",
                (c["prompt"], c["chosen"], c["rejected"],
                 round(margin_for(c["score"], a.threshold, a.floor), 4), now, SOURCE))
            inserted += 1
        con.commit()
        con.close()

    novel = sum(1 for c in capped if not c["judge_detected"])
    mode = "LIVE" if a.live else "SHADOW"
    print(f"BINOCULARS_TO_DPO_DONE mode={mode} candidates={stats['selected']} "
          f"capped_to={len(capped)} over_cap={n_over_cap} inserted={inserted} "
          f"skipped_dup={skipped_dup} judge_fooled_but_flagged={novel} "
          f"threshold={a.threshold}")
    print(f"  filters: trials={stats['trials']} no_score={stats['no_score']} "
          f"above_threshold={stats['above_threshold']} too_short={stats['too_short']} "
          f"judge_fooled_skipped={stats['judge_fooled_skipped']} blank={stats['blank']} "
          f"corrupt_chosen={stats['corrupt_chosen']}")
    if stats["corrupt_chosen"]:
        print(f"  NOTE: {stats['corrupt_chosen']} pair(s) refused — real_seth carries the "
              "leading-byte extraction artifact (~20% of ground_truth.jsonl as of "
              "2026-07-25). Fix the extractor to recover them.")
    if dup_sources:
        print(f"  dedup hits by existing source: {dup_sources}")
    if not a.live:
        print("  SHADOW: nothing written to the DB. Re-run with --live to insert "
              "(confirm the threshold matches the CURRENT adapter first).")


# -------------------------------------------------------------- selftest ----

def selftest():
    m = margin_for(0.9643, 0.9643, 0.60)
    assert abs(m) < 1e-9, m                      # at threshold -> 0
    assert margin_for(0.50, 0.9643, 0.60) == 1.0  # below floor -> clamped 1
    assert abs(margin_for(0.60, 0.9643, 0.60) - 1.0) < 1e-9
    mid = margin_for((0.9643 + 0.60) / 2, 0.9643, 0.60)
    assert abs(mid - 0.5) < 1e-9, mid
    assert margin_for(1.5, 0.9643, 0.60) == 0.0   # above threshold -> clamped 0

    doc = {
        "binoculars": {"ai_by_trial": {
            "0": {"score": 0.62, "n_tokens": 10},   # flagged, judge fooled
            "1": {"score": 0.90, "n_tokens": 10},   # flagged, judge caught
            "2": {"score": 1.20, "n_tokens": 10},   # above threshold
            "3": {"score": 0.70, "n_tokens": 2},    # too short
            "4": {"score": 0.70, "n_tokens": 10},   # blank chosen
        }},
        "trials": [
            {"i": 0, "incoming": "yo", "real_seth": "hey", "ai_response": "greetings",
             "judge_correct": False},
            {"i": 1, "incoming": "sup", "real_seth": "nm u", "ai_response": "not much!",
             "judge_correct": True},
            {"i": 2, "incoming": "ok", "real_seth": "k", "ai_response": "sounds good",
             "judge_correct": False},
            {"i": 3, "incoming": "hi", "real_seth": "yo", "ai_response": "hi",
             "judge_correct": False},
            {"i": 4, "incoming": "?", "real_seth": "   ", "ai_response": "indeed",
             "judge_correct": False},
            {"i": 5, "incoming": "no score", "real_seth": "a", "ai_response": "b"},
        ],
    }
    c, s = select_candidates(doc, 0.9643, 4, False)
    assert [x["trial"] for x in c] == [0, 1], c   # sorted most-machine first
    assert s == {"trials": 6, "no_score": 1, "above_threshold": 1, "too_short": 1,
                 "judge_fooled_skipped": 0, "blank": 1, "corrupt_chosen": 0,
                 "selected": 2}, s
    assert c[0]["chosen"] == "hey" and c[0]["rejected"] == "greetings"

    # leading-byte guard: real observed artifacts must be refused, clean text kept
    for bad in ("%A ton, you know I got feelings", "0Oh hey there! Jesus",
                "HYeah works for me", ",I don't know if I have",
                ".Edison and my brother", ";No he doesn't want to"):
        assert looks_corrupted(bad), bad
    for ok in ("hey", "yeah works for me", "Same!", "Ok", "I appreciate you",
               "...", "😂", "", "3 people", "OK sounds good"):
        assert not looks_corrupted(ok), ok

    doc_c = {"binoculars": {"ai_by_trial": {"0": {"score": 0.62, "n_tokens": 10}}},
             "trials": [{"i": 0, "incoming": "yo", "real_seth": "%A ton, you know",
                         "ai_response": "greetings", "judge_correct": False}]}
    cc, sc = select_candidates(doc_c, 0.9643, 4, False)
    assert cc == [] and sc["corrupt_chosen"] == 1, (cc, sc)
    cc2, _ = select_candidates(doc_c, 0.9643, 4, False, allow_corrupt=True)
    assert len(cc2) == 1, cc2   # escape hatch still works

    c2, s2 = select_candidates(doc, 0.9643, 4, True)   # AND-mode
    assert [x["trial"] for x in c2] == [1], c2
    # trials 0 and 4 both drop here: the judge filter precedes the blank check,
    # so trial 4 is counted as judge-skipped rather than blank.
    assert s2["judge_fooled_skipped"] == 2 and s2["blank"] == 0, s2

    # dedup is cross-source
    con = sqlite3.connect(":memory:")
    con.execute(DPO_PAIRS_DDL)
    con.execute("INSERT INTO dpo_pairs(prompt, chosen, rejected, margin, timestamp, source) "
                "VALUES('yo','hey','greetings',0.5,1,'blind_ab_judge')")
    assert already_present(con, "yo", "hey", "greetings") == "blind_ab_judge"
    assert already_present(con, "yo", "hey", "different") is None
    print("selftest OK")


if __name__ == "__main__":
    main()
