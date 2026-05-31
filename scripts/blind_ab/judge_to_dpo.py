#!/usr/bin/env python3
"""B3 reward wire: turn blind-A/B judge verdicts into corrective DPO pairs.

This closes the measurement→training loop. Every A/B round the judge could
distinguish h-uman from the real Seth is a LOSS — and exactly the signal we
want to train against: chosen = the real Seth reply, rejected = h-uman's
distinguishable reply, prompt = the conversation context.

Pipeline position:
    export_seth_triples.py -> make_rating_sheet.py -> synthetic_judge.py
      -> [score.py for the metric]   AND   -> judge_to_dpo.py (this)  -> dpo_pairs

DIFFERENT-FAMILY-ONLY GATE (hard): only verdicts from a different model family
than h-uman's generator may become training pairs. Same-family judges (the
local gemma that also generates) inflate via shared blindspot — empirically
gemma self-judge 0.21 vs Opus 0.90 on the same triples. Feeding same-family
verdicts back would train h-uman to fool a judge that already can't see it,
i.e. reward hacking. We therefore accept ONLY rows whose `judge_api` is in
ALLOWED_JUDGE_FAMILIES (anthropic = Opus, cloud). Local-gemma rows are
refused with a visible count.

Only LOSSES (judge correctly picked the real Seth) produce a corrective pair.
Rounds h-uman won (judge fooled) need no correction and are skipped.

Usage:
    python3 judge_to_dpo.py completed_sheet.csv \
        --key answer_key.json [--db ~/.human/memory.db] [--dry-run]

The completed sheet must come from synthetic_judge.py (carries judge_api,
judge_model, choice, confidence, context, option_A, option_B, row_id).
"""
import argparse, csv, json, os, sqlite3, sys, time

# Only these judge families may feed the training loop (different family from
# the local gemma generator). Extend deliberately, never to include the
# generator's own family.
ALLOWED_JUDGE_FAMILIES = {"anthropic"}

DPO_PAIRS_DDL = (
    "CREATE TABLE IF NOT EXISTS dpo_pairs("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "prompt TEXT, chosen TEXT, rejected TEXT, "
    "margin REAL, timestamp INTEGER, source TEXT)"
)
SOURCE = "blind_ab_judge"


def row_id(r):
    return r.get("row_id") or r.get("id") or r.get("context", "")[:64]


def already_present(con, prompt, chosen, rejected):
    """Content-level dedup so re-running the same sheet is idempotent."""
    cur = con.execute(
        "SELECT 1 FROM dpo_pairs WHERE source=? AND prompt=? AND chosen=? AND rejected=? LIMIT 1",
        (SOURCE, prompt, chosen, rejected))
    return cur.fetchone() is not None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet", help="completed sheet from synthetic_judge.py")
    ap.add_argument("--key", required=True, help="answer_key.json: row_id -> 'A'|'B' (real Seth)")
    ap.add_argument("--db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--dry-run", action="store_true", help="report, do not insert")
    a = ap.parse_args()

    key = json.load(open(a.key))
    rows = list(csv.DictReader(open(a.sheet)))
    if not rows:
        sys.exit("empty sheet")

    con = None
    if not a.dry_run:
        con = sqlite3.connect(a.db)
        con.execute(DPO_PAIRS_DDL)

    inserted = skipped_family = skipped_won = skipped_blank = skipped_dup = skipped_nokey = 0
    for r in rows:
        rid = row_id(r)
        family = (r.get("judge_api") or "").strip().lower()
        # HARD different-family gate.
        if family not in ALLOWED_JUDGE_FAMILIES:
            skipped_family += 1
            continue
        choice = (r.get("choice") or "").strip()
        if choice not in ("A", "B"):
            skipped_blank += 1  # judge abstained / parse failure
            continue
        real = key.get(rid)
        if real not in ("A", "B"):
            skipped_nokey += 1
            continue
        if choice != real:
            skipped_won += 1  # judge fooled -> h-uman indistinguishable -> no correction
            continue
        # LOSS: chosen = real Seth, rejected = h-uman.
        seth_text = r["option_A"] if real == "A" else r["option_B"]
        huuman_text = r["option_B"] if real == "A" else r["option_A"]
        prompt = r.get("context", "")
        if not seth_text or not huuman_text:
            skipped_blank += 1
            continue
        try:
            conf = float(r.get("confidence") or 3)
        except ValueError:
            conf = 3.0
        margin = max(0.0, min(1.0, conf / 5.0))  # confidence 1..5 -> 0.2..1.0
        if a.dry_run:
            inserted += 1
            continue
        if already_present(con, prompt, seth_text, huuman_text):
            skipped_dup += 1
            continue
        con.execute(
            "INSERT INTO dpo_pairs(prompt, chosen, rejected, margin, timestamp, source) "
            "VALUES(?,?,?,?,?,?)",
            (prompt, seth_text, huuman_text, margin, int(time.time()), SOURCE))
        inserted += 1

    if con is not None:
        con.commit()
        con.close()

    print(f"JUDGE_TO_DPO_DONE inserted={inserted} "
          f"skipped_family={skipped_family} skipped_won={skipped_won} "
          f"skipped_blank={skipped_blank} skipped_dup={skipped_dup} "
          f"skipped_nokey={skipped_nokey} dry_run={a.dry_run}")
    if skipped_family and inserted == 0:
        print("NOTE: all rows were same-family (local) judge verdicts — refused by the "
              "different-family gate. Re-judge with synthetic_judge.py --api anthropic "
              "to produce trainable pairs.", file=sys.stderr)


if __name__ == "__main__":
    main()
