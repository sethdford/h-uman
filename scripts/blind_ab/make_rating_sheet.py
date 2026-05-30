#!/usr/bin/env python3
"""Build a blind 2AFC rating sheet from triples.json.

Each triple {id, context, seth_reply, huuman_reply} becomes one row with two
unlabeled options (A/B) in randomized order. Emits:
  - rating_sheet.csv : columns id, context, option_A, option_B, choice, confidence
                       (raters fill `choice` = A|B and `confidence` = 1..5)
  - answer_key.json  : { id: "A"|"B" }  where the value is the SETH (real) option.
                       Keep this private; raters must never see it.

Detection (in score.py) = rater picked the Seth option. 0.5 == indistinguishable.

Usage:
    python3 make_rating_sheet.py triples.json [--seed 42] [--out-dir .]
"""
import argparse, csv, json, random, sys, os


def build(triples, seed):
    rng = random.Random(seed)
    rows, key = [], {}
    for t in triples:
        for f in ("id", "context", "seth_reply", "huuman_reply"):
            if f not in t:
                raise ValueError(f"triple missing field '{f}': {t!r}")
        seth_is_A = rng.random() < 0.5
        opt_a = t["seth_reply"] if seth_is_A else t["huuman_reply"]
        opt_b = t["huuman_reply"] if seth_is_A else t["seth_reply"]
        key[t["id"]] = "A" if seth_is_A else "B"   # which option is the REAL Seth
        rows.append({"id": t["id"], "context": t["context"],
                     "option_A": opt_a, "option_B": opt_b,
                     "choice": "", "confidence": ""})
    rng.shuffle(rows)
    return rows, key


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("triples")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out-dir", default=".")
    a = ap.parse_args()
    with open(a.triples) as f:
        triples = json.load(f)
    if not isinstance(triples, list) or not triples:
        print("triples.json must be a non-empty list", file=sys.stderr); sys.exit(2)
    rows, key = build(triples, a.seed)
    os.makedirs(a.out_dir, exist_ok=True)
    sheet = os.path.join(a.out_dir, "rating_sheet.csv")
    keyf = os.path.join(a.out_dir, "answer_key.json")
    with open(sheet, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "option_A", "option_B",
                                          "choice", "confidence"])
        w.writeheader(); w.writerows(rows)
    with open(keyf, "w") as f:
        json.dump(key, f, indent=2)
    print(f"wrote {sheet} ({len(rows)} items) and {keyf} (KEEP PRIVATE)")


if __name__ == "__main__":
    main()
