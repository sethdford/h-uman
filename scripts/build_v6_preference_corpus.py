#!/usr/bin/env python3
"""Assemble the seth-glm-air-v6 preference corpus.

v6 is the ladder's preference round (roadmap `plans/2026-07-11-adapter-v5-roadmap.md`).
It targets the three failure modes HUMAN blind raters identified on the v5 arm --
NOT the stale-fact premise, which `docs/research/v6-corpus-regeneration-scope-20260727.md`
measured and ruled out (0.7% corpus contamination).

    1. OVER-ELABORATION   -- the dominant tell. Measured on the 160 cycle-4 items:
                             the model's reply is longer than Seth's in 82% of cases,
                             median length ratio 2.25x, p90 7.67x.
    2. REGISTER / WARMTH  -- flat where Seth is warm or playful.
    3. ASSISTANT-DISCLOSURE -- capability talk and scheduling-assistant register.

SOURCE ADMISSION IS DEFAULT-DENY. Every candidate source was audited; the ones
admitted are listed in ADMITTED with the evidence that admitted them, and the ones
rejected are in REJECTED with the measurement that rejected them. Adding a source
means adding it here with its audit, not loosening a filter.

Output (default ~/.human/training-data/glm-v6-pref/):
    train.jsonl / valid.jsonl  -- {"prompt","chosen","rejected"} per row
    manifest.json              -- counts, per-source provenance, and the list of
                                  cycle-4 ids consumed (the cycle-5 eval MUST
                                  exclude these or it measures memorisation)
"""
import argparse
import json
import os
import random
import sqlite3
import sys
from pathlib import Path

HOME = Path(os.path.expanduser("~"))
CYCLE4_TRIPLES = HOME / "blind_ab_run/cycle4-20260726/triples.json"
MEMORY_DB = HOME / ".human/memory.db"

# --- Admitted dpo_pairs sources -------------------------------------------------
# Audited 2026-07-27 against the whole table (700 rows).
ADMITTED = {
    "generated_v2": "217 rows. chosen=terse Seth register, rejected=generic "
                    "assistant prose. Squarely targets modes 1 and 3.",
    "arena": "156 rows. Self-play arena; chosen is consistently the terser, "
             "less self-referential of the two. Targets modes 1 and 2.",
}

# --- Rejected dpo_pairs sources -------------------------------------------------
# Each rejection is a measurement, not a preference. Re-run the audit in
# scripts/audit_dpo_pairs.py before re-admitting any of these.
REJECTED = {
    "auto_correction": "118 rows, but 109 are EXACT duplicates of outbound_edit "
                       "and 21% show the sliding-window signature (prompt[N] == "
                       "chosen[N-1]) -- 'chosen' is the NEXT thread message, not "
                       "a better reply. Training this teaches non-sequiturs.",
    "outbound_edit": "110 rows, 109 duplicated with auto_correction, same "
                     "sliding-window mislabel.",
    "user_feedback": "51 rows, 51/51 have an empty chosen or rejected side. "
                     "Empty-chosen into a preference objective is exactly the "
                     "2026-05 ORPO blank-output collapse.",
    "implicit_feedback": "38 rows, inverted in inspection -- assistant-register "
                         "replies appear on the chosen side.",
    "reflection_retry": "10 rows of echoes, literal 'GOOD', and a truncated "
                        "fragment as chosen.",
}

# --- Curated cycle-4 gold -------------------------------------------------------
# 75 of the 160 cycle-4 items tripped a target-mode detector; all 75 were then read
# by hand because the detector cannot distinguish "the model over-elaborated" from
# "Seth's logged next message answered a DIFFERENT message". `export_seth_triples.py`
# pairs a context with Seth's next SENT message, and in real threads that is often a
# non-sequitur. 42 kept / 33 dropped.
CYCLE4_KEEP = [
    "c4-005", "c4-008", "c4-014", "c4-015", "c4-016", "c4-021", "c4-022",
    "c4-027", "c4-028", "c4-032", "c4-043", "c4-044", "c4-050", "c4-052",
    "c4-053", "c4-059", "c4-063", "c4-070", "c4-071", "c4-075", "c4-076",
    "c4-079", "c4-080", "c4-081", "c4-082", "c4-083", "c4-090", "c4-092",
    "c4-093", "c4-094", "c4-113", "c4-115", "c4-117", "c4-121", "c4-125",
    "c4-126", "c4-129", "c4-135", "c4-138", "c4-142", "c4-147", "c4-160",
]

# Why each dropped item was dropped -- kept so the judgement is auditable and so a
# future curator does not silently re-admit a poisoned pair.
CYCLE4_DROP = {
    "context_mismatch": [
        "c4-009", "c4-019", "c4-029", "c4-040", "c4-057", "c4-066", "c4-068",
        "c4-072", "c4-077", "c4-078", "c4-099", "c4-102", "c4-105", "c4-108",
        "c4-118", "c4-123", "c4-131", "c4-137", "c4-146", "c4-150", "c4-036",
    ],
    "export_artifact_blank_image": [
        # Context is a stripped image attachment. The model correctly says it
        # cannot see an image; Seth answered the picture. Not a failure.
        "c4-012", "c4-041", "c4-047", "c4-056", "c4-095",
    ],
    "grounding_not_elaboration": [
        # Model fabricated a fact. Real bug, wrong training signal -- the fix is
        # grounding, and 'chosen' here would teach a different false assertion.
        "c4-013",
    ],
    "model_reply_is_better": [
        # Rejecting these would train warmth OUT.
        "c4-155", "c4-023",
    ],
    "ai_self_disclosure_ambiguous": [
        # "is this an AI?" prompts. Seth's real replies concede the bot; the
        # model's denial may be the wanted behaviour. Not ours to settle here.
        "c4-046", "c4-096",
    ],
    "weak_preference_signal": [
        # Seth's own reply is long too, so the pair does not teach brevity.
        "c4-035", "c4-055",
    ],
}


def norm(s):
    return (s or "").strip()


def load_cycle4(path):
    rows, keep = [], set(CYCLE4_KEEP)
    triples = json.load(open(path))
    by_id = {t["id"]: t for t in triples}
    missing = keep - set(by_id)
    if missing:
        raise SystemExit(f"FATAL: curated ids absent from {path}: {sorted(missing)}")
    for tid in CYCLE4_KEEP:
        t = by_id[tid]
        rows.append({
            "prompt": norm(t["context"]),
            "chosen": norm(t["seth_reply"]),
            "rejected": norm(t["huuman_reply"]),
            "_src": "cycle4_curated",
            "_id": tid,
        })
    return rows


def load_dpo_pairs(db_path):
    db = sqlite3.connect(str(db_path))
    rows, seen = [], set()
    for src in ADMITTED:
        for p, c, r in db.execute(
                "SELECT prompt,chosen,rejected FROM dpo_pairs WHERE source=?", (src,)):
            p, c, r = norm(p), norm(c), norm(r)
            if not c or not r or c == r:
                continue
            key = (p, c, r)
            if key in seen:
                continue
            seen.add(key)
            rows.append({"prompt": p, "chosen": c, "rejected": r, "_src": src, "_id": ""})
    return rows


def validate(rows, floor):
    """Refuse to emit a corpus that cannot support the claim made of it."""
    problems = []
    for i, r in enumerate(rows):
        if not r["chosen"] or not r["rejected"]:
            problems.append(f"row {i} ({r['_src']}): empty chosen/rejected")
        if r["chosen"] == r["rejected"]:
            problems.append(f"row {i} ({r['_src']}): chosen == rejected")
    if problems:
        raise SystemExit("FATAL: degenerate rows:\n  " + "\n  ".join(problems[:20]))
    if len(rows) < floor:
        raise SystemExit(
            f"FATAL: {len(rows)} pairs < floor {floor}. Training on this would be "
            f"training on noise. Widen the audited sources or stop.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", default=str(HOME / ".human/training-data/glm-v6-pref"))
    ap.add_argument("--cycle4", default=str(CYCLE4_TRIPLES))
    ap.add_argument("--db", default=str(MEMORY_DB))
    ap.add_argument("--valid-frac", type=float, default=0.08)
    ap.add_argument("--floor", type=int, default=200,
                    help="Refuse to emit fewer pairs than this.")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    gold = load_cycle4(Path(args.cycle4))
    synth = load_dpo_pairs(Path(args.db))
    rows = gold + synth
    validate(rows, args.floor)

    rng = random.Random(args.seed)
    rng.shuffle(rows)
    n_val = max(8, int(len(rows) * args.valid_frac))
    valid, train = rows[:n_val], rows[n_val:]

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for name, part in (("train", train), ("valid", valid)):
        with open(out / f"{name}.jsonl", "w") as fh:
            for r in part:
                fh.write(json.dumps({k: r[k] for k in ("prompt", "chosen", "rejected")}) + "\n")

    by_src = {}
    for r in rows:
        by_src[r["_src"]] = by_src.get(r["_src"], 0) + 1
    manifest = {
        "built": "scripts/build_v6_preference_corpus.py",
        "targets": ["over_elaboration", "register_warmth", "assistant_disclosure"],
        "counts": {"total": len(rows), "train": len(train), "valid": len(valid)},
        "by_source": by_src,
        "admitted_sources": ADMITTED,
        "rejected_sources": REJECTED,
        # The cycle-5 eval MUST exclude these ids -- they are now training data.
        "cycle4_ids_consumed": CYCLE4_KEEP,
        "cycle4_dropped": CYCLE4_DROP,
        "seed": args.seed,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))

    print(f"[v6-corpus] wrote {out}")
    for k, v in sorted(by_src.items(), key=lambda kv: -kv[1]):
        print(f"  {k:20} {v:>4}")
    print(f"  {'TOTAL':20} {len(rows):>4}  (train {len(train)} / valid {len(valid)})")
    print(f"[v6-corpus] cycle-4 ids consumed: {len(CYCLE4_KEEP)} "
          f"(exclude ALL of these from the cycle-5 eval)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
