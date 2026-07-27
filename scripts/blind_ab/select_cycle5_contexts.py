#!/usr/bin/env python3
"""Select the cycle-5 blind-A/B context set for the seth-glm-air-v6 candidate arm.

EXCLUSIONS ARE THE POINT OF THIS SCRIPT. A context is disqualified if it is:

  * consumed by v6 training  -- the 42 curated cycle-4 pairs in the v6 corpus
    manifest. Measuring v6 on text it was trained on measures memorisation.
  * in the PENDING cycle-4 human sheet -- those 24 items are an open
    measurement; reusing them entangles two cycles' verdicts.
  * from an earlier cycle namespace (cyc3_*, bab_*, bab00*).
  * documented during v6 curation as context_mismatch or a blank-image export
    artifact. `export_seth_triples.py` pairs a context with Seth's next SENT
    message, which in real threads often answers something else -- so the
    "real Seth reply" is a non-sequitur and the item tests nothing about voice.
    Only the two categories with an objective defect are excluded; pairs dropped
    from TRAINING for judgement reasons (model reply was better, weak preference
    signal, AI-disclosure ambiguity) remain valid A/B items and stay eligible.

Stratification: the three target failure modes surface in different registers --
over-elaboration shows most on SHORT prompts (3 words suffice, model writes 40),
warmth mismatch on PERSONAL ones, disclosure on LOGISTICS/professional ones. A
flat random draw can miss a whole stratum at n=16, so the sample is drawn per
stratum and only then shuffled.
"""
import argparse
import csv
import json
import os
import random
import re
from pathlib import Path

HOME = Path(os.path.expanduser("~"))
EXCLUDED_PREFIXES = ("cyc3_", "bab_", "bab00")

WARM = re.compile(r"(love|miss|heart|proud|congrat|sorry|hope you|feel better|"
                  r"beautiful|handsome|babe|thank you|😂|🙏|❤|😅)", re.I)
LOGISTICS = re.compile(r"(schedul|appointment|address|invoice|payment|deliver|"
                       r"closing|book|confirm|reschedul|available|call me|"
                       r"send you|paperwork|sign)", re.I)


def stratum(ctx):
    if LOGISTICS.search(ctx):
        return "logistics"
    if WARM.search(ctx):
        return "personal"
    return "short" if len(ctx) < 60 else "general"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--triples", default=str(HOME / "blind_ab_run/cycle4-20260726/triples.json"))
    ap.add_argument("--manifest", default=str(HOME / ".human/training-data/glm-v6-pref/manifest.json"))
    ap.add_argument("--pending-sheet", default=str(HOME / "blind_ab_run/cycle4-20260726/rating_sheet_human24.csv"))
    ap.add_argument("--n", type=int, default=16)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    triples = json.load(open(args.triples))
    manifest = json.load(open(args.manifest))
    trained = set(manifest["cycle4_ids_consumed"])
    pending = {r["id"] for r in csv.DictReader(open(args.pending_sheet))}
    dropped = manifest.get("cycle4_dropped", {})
    defective = set(dropped.get("context_mismatch", [])) | \
        set(dropped.get("export_artifact_blank_image", []))

    pool = []
    for t in triples:
        tid = t["id"]
        if tid in trained or tid in pending or tid in defective:
            continue
        if tid.startswith(EXCLUDED_PREFIXES):
            continue
        if not (t.get("seth_reply") or "").strip():
            continue
        pool.append(t)

    strata = {}
    for t in pool:
        strata.setdefault(stratum(t["context"]), []).append(t)

    rng = random.Random(args.seed)
    # Round-robin across strata so every register is represented at n=16.
    for v in strata.values():
        v.sort(key=lambda t: t["id"])
        rng.shuffle(v)
    picked, order = [], sorted(strata)
    while len(picked) < args.n and any(strata[k] for k in order):
        for k in order:
            if strata[k] and len(picked) < args.n:
                picked.append(strata[k].pop())
    rng.shuffle(picked)

    out = [{"id": f"c5-{i+1:03d}", "src_id": t["id"], "context": t["context"],
            "seth_reply": t["seth_reply"]} for i, t in enumerate(picked)]
    Path(args.out).write_text(json.dumps(out, indent=2, ensure_ascii=False))

    print(f"[cycle5] pool after exclusions: {len(pool)} "
          f"(excluded {len(trained)} trained + {len(pending)} pending-sheet)")
    print(f"[cycle5] strata: { {k: len(v) for k, v in sorted(strata.items())} } (post-draw remainder)")
    from collections import Counter
    print(f"[cycle5] picked {len(out)}: {dict(Counter(stratum(t['context']) for t in picked))}")
    print(f"[cycle5] wrote {args.out}")


if __name__ == "__main__":
    main()
