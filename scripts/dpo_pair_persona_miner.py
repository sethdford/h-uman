#!/usr/bin/env python3
"""
Re-score every (prompt, chosen, rejected) row in dpo_pairs with the trained
speaker-ID classifier. Surface training-data quality bugs:

  - INVERTED pairs: rejected has HIGHER P(Seth) than chosen — these are
    mislabeled. Training ORPO on inverted pairs teaches the model to pick
    the LESS Seth-shape response. We want to flag and exclude.

  - NARROW pairs:  |P(chosen) - P(rejected)| < margin_threshold — these
    are low-information; both are similarly Seth-shape (or neither is).
    Burns training compute for negligible signal.

  - SUPPORTING pairs: chosen > rejected by a healthy margin (>=0.15).
    The pairs we want training to rely on.

Optionally writes:
  - A new column `p_seth_margin` to dpo_pairs (chosen - rejected).
  - Or a sidecar JSON for analysis without mutating the table.

This is the "do something with the L1 PersonaEval insight" application:
turn a free-running classifier into a corpus-cleaning tool BEFORE the
ORPO run starts.

Usage:
  python3 scripts/dpo_pair_persona_miner.py
  python3 scripts/dpo_pair_persona_miner.py --write-margin --db ~/.human/memory.db
  python3 scripts/dpo_pair_persona_miner.py --source-filter imessage_tapback
"""

import argparse
import json
import sqlite3
import sys
import statistics
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

DB_DEFAULT = str(Path.home() / ".human" / "memory.db")
CLF_DEFAULT = "/tmp/seth_speaker_id.json"


def fetch_pairs(db_path: str, source_filter: str = None) -> list:
    con = sqlite3.connect(db_path)
    # Some legacy rows have non-UTF-8 bytes (binary blobs from misrouted
    # writes); read as bytes and decode with replace so we don't blow up.
    con.text_factory = bytes
    q = "SELECT id, prompt, chosen, rejected, source FROM dpo_pairs"
    args = []
    if source_filter:
        q += " WHERE source = ?"
        args.append(source_filter.encode("utf-8"))
    q += " ORDER BY id"
    rows = con.execute(q, args).fetchall()
    con.close()

    def _decode(v):
        if v is None:
            return None
        if isinstance(v, bytes):
            return v.decode("utf-8", errors="replace")
        return v
    return [{"id": r[0], "prompt": _decode(r[1]), "chosen": _decode(r[2]),
             "rejected": _decode(r[3]), "source": _decode(r[4])}
            for r in rows]


def add_margin_column_if_missing(db_path: str) -> bool:
    """Idempotent: ensure dpo_pairs has p_seth_margin REAL."""
    con = sqlite3.connect(db_path)
    cur = con.execute("PRAGMA table_info(dpo_pairs)")
    cols = {row[1] for row in cur.fetchall()}
    added = False
    if "p_seth_chosen" not in cols:
        con.execute("ALTER TABLE dpo_pairs ADD COLUMN p_seth_chosen REAL")
        added = True
    if "p_seth_rejected" not in cols:
        con.execute("ALTER TABLE dpo_pairs ADD COLUMN p_seth_rejected REAL")
        added = True
    if "p_seth_margin" not in cols:
        con.execute("ALTER TABLE dpo_pairs ADD COLUMN p_seth_margin REAL")
        added = True
    con.commit()
    con.close()
    return added


def write_margins(db_path: str, scored: list):
    """Write back per-row p_seth_chosen, p_seth_rejected, p_seth_margin."""
    con = sqlite3.connect(db_path)
    n = 0
    for r in scored:
        con.execute(
            "UPDATE dpo_pairs SET p_seth_chosen=?, p_seth_rejected=?, "
            "p_seth_margin=? WHERE id=?",
            (r["p_seth_chosen"], r["p_seth_rejected"], r["margin"], r["id"]),
        )
        n += 1
    con.commit()
    con.close()
    return n


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--db", default=DB_DEFAULT)
    p.add_argument("--classifier", default=CLF_DEFAULT)
    p.add_argument("--source-filter",
                   help="Only score rows with this source value")
    p.add_argument("--narrow-threshold", type=float, default=0.10,
                   help="|margin| < this is 'narrow'")
    p.add_argument("--out", default="/tmp/dpo_persona_audit.json")
    p.add_argument("--write-margin", action="store_true",
                   help="Add p_seth_* columns and write per-row")
    args = p.parse_args()

    from personaeval_speaker_id import load_classifier, p_seth
    clf = load_classifier(args.classifier)

    pairs = fetch_pairs(args.db, args.source_filter)
    print(f"Scoring {len(pairs)} dpo_pairs rows"
          + (f" (source={args.source_filter})" if args.source_filter else ""))

    scored = []
    for pair in pairs:
        pc = p_seth(clf, pair["chosen"] or "")
        pr = p_seth(clf, pair["rejected"] or "")
        margin = pc - pr
        scored.append({
            **pair,
            "p_seth_chosen": pc,
            "p_seth_rejected": pr,
            "margin": margin,
        })

    # Buckets
    inverted = [r for r in scored if r["margin"] < 0]
    narrow = [r for r in scored
              if abs(r["margin"]) < args.narrow_threshold and r["margin"] >= 0]
    supporting = [r for r in scored if r["margin"] >= args.narrow_threshold]

    # Per-source health
    by_source = {}
    for r in scored:
        by_source.setdefault(r["source"], []).append(r)

    print()
    print("=== Pair quality buckets ===")
    print(f"  inverted  (P(chosen) < P(rejected)):  {len(inverted):>4} / {len(scored):>4} "
          f"({100*len(inverted)/len(scored):.1f}%)")
    print(f"  narrow    (|margin| < {args.narrow_threshold}):           "
          f"{len(narrow):>4} / {len(scored):>4} "
          f"({100*len(narrow)/len(scored):.1f}%)")
    print(f"  supporting (margin >= {args.narrow_threshold}):           "
          f"{len(supporting):>4} / {len(scored):>4} "
          f"({100*len(supporting)/len(scored):.1f}%)")

    if scored:
        margins = [r["margin"] for r in scored]
        print()
        print(f"  margin distribution: "
              f"mean={statistics.mean(margins):+.3f}, "
              f"median={statistics.median(margins):+.3f}, "
              f"min={min(margins):+.3f}, max={max(margins):+.3f}")

    print()
    print("=== Per-source quality ===")
    for source, rows in sorted(by_source.items(),
                               key=lambda kv: -len(kv[1])):
        inv = sum(1 for r in rows if r["margin"] < 0)
        sup = sum(1 for r in rows if r["margin"] >= args.narrow_threshold)
        m = statistics.mean([r["margin"] for r in rows])
        print(f"  {source:<24} n={len(rows):>4}  "
              f"mean_margin={m:+.3f}  "
              f"inverted={inv:>3}  supporting={sup:>3}")

    print()
    if inverted:
        print(f"=== {min(5, len(inverted))} most-inverted pairs (training poison) ===")
        for r in sorted(inverted, key=lambda x: x["margin"])[:5]:
            print(f"  id={r['id']:<5} margin={r['margin']:+.3f}  "
                  f"source={r['source']}")
            print(f"    prompt:   {(r['prompt'] or '')[:80]!r}")
            print(f"    chosen   P={r['p_seth_chosen']:.3f}: "
                  f"{(r['chosen'] or '')[:80]!r}")
            print(f"    rejected P={r['p_seth_rejected']:.3f}: "
                  f"{(r['rejected'] or '')[:80]!r}")

    report = {
        "total": len(scored),
        "inverted": len(inverted),
        "narrow": len(narrow),
        "supporting": len(supporting),
        "by_source": {
            s: {
                "n": len(rows),
                "mean_margin": statistics.mean([r["margin"] for r in rows]),
                "inverted": sum(1 for r in rows if r["margin"] < 0),
                "supporting": sum(1 for r in rows
                                  if r["margin"] >= args.narrow_threshold),
            } for s, rows in by_source.items()
        },
        "inverted_samples": sorted(inverted, key=lambda x: x["margin"])[:20],
    }
    Path(args.out).write_text(json.dumps(report, indent=2))
    print()
    print(f"Full audit: {args.out}")

    if args.write_margin:
        print()
        print("Writing p_seth_* columns to dpo_pairs...")
        add_margin_column_if_missing(args.db)
        n = write_margins(args.db, scored)
        print(f"  wrote margins for {n} rows")

    # Helpful exit code: 1 if >5% of pairs are inverted (training corpus
    # is poisoned); 0 otherwise.
    if scored and len(inverted) / len(scored) > 0.05:
        print(f"\nWARNING: {100*len(inverted)/len(scored):.1f}% of pairs "
              f"are inverted — corpus quality is poor for ORPO training.")
        sys.exit(1)


if __name__ == "__main__":
    main()
