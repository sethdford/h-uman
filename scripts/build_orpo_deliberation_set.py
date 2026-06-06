#!/usr/bin/env python3
"""
Build a consolidated ORPO preference set targeting deliberation suppression.

Context (2026-06-05): the served gemma-4-31b + seth-lora-v4-repair adapter was
trained to deliberate — it emits ~120-340 tokens of markdown-bullet reasoning
before the terse visible reply, and ignores the no-think instruction. ORPO
(odds-ratio preference optimization, reference-free) over {prompt, chosen,
rejected} pairs where `rejected` is the deliberation+reply and `chosen` is the
terse reply directly PENALIZES the preamble, unlike SFT-on-chosen which only
rewards brevity.

The preference data already exists across several files in different schemas;
this script normalizes, quality-filters, dedups, and splits them into one ORPO
train/valid set that TRL's ORPOTrainer consumes directly (prompt/chosen/rejected
columns).

Sources & schema mapping (only reply-generation pairs with a VALID chosen):
  - m3-alpaca-dpo-*.jsonl        {prompt, chosen, rejected}
  - m3-rewrite-pairs.jsonl       {prompt, rejected, accepted}  -> chosen=accepted
                                 (the deliberation-gold source)
  - m3-combined-dpo-*.jsonl      {prompt, chosen, rejected}    (verbose rejected)
EXCLUDED (wrong task / no valid chosen):
  - m3-dpo-rejections-*.jsonl    chosen is usually "None" (guard rejected, no
                                 accepted reply captured) -> unusable as a pair
  - dpo_finetune/*.jsonl         grader/scoring task ("Score it as GOOD..."),
                                 empty rejected -> not reply generation

Quality filters (all sources): non-empty prompt/chosen/rejected after strip;
chosen not in {none,null,""}; chosen != rejected; chosen length sane (a text
reply, not a grader transcript); dedup on normalized (prompt, chosen, rejected).

Usage:
    python3 scripts/build_orpo_deliberation_set.py            # build + stats
    python3 scripts/build_orpo_deliberation_set.py --out DIR  # custom out dir
    python3 scripts/build_orpo_deliberation_set.py --dry-run  # stats only

Exit codes: 0 ok | 2 no usable pairs produced
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys

TRAIN_DATA = os.path.expanduser("~/.human/training-data")

# (glob, prompt_key, chosen_key, rejected_key)
SOURCES = [
    (os.path.join(TRAIN_DATA, "m3-alpaca-dpo-*.jsonl"), "prompt", "chosen", "rejected"),
    (os.path.join(TRAIN_DATA, "m3-rewrite-pairs.jsonl"), "prompt", "accepted", "rejected"),
    (os.path.join(TRAIN_DATA, "m3-combined-dpo-*.jsonl"), "prompt", "chosen", "rejected"),
]

INVALID_CHOSEN = {"", "none", "null", "n/a", "na"}
MAX_CHOSEN_CHARS = 600   # a text reply, not a grader transcript / essay
MAX_PROMPT_CHARS = 4000
MAX_REJECTED_CHARS = 8000


def _norm(s: str) -> str:
    return " ".join(str(s or "").split()).casefold()


def _is_deliberation(rejected: str) -> bool:
    """Heuristic: does the rejected text carry the bullet/channel-deliberation
    shape (vs. e.g. a verbose-rewrite counterfactual)? Used for stats only."""
    r = rejected or ""
    return bool(
        "<|channel>thought" in r
        or re.search(r"(?m)^\s*\*\s", r)          # markdown bullets
        or r.lstrip().lower().startswith(("the user", "user:", "current time"))
    )


def _valid(prompt: str, chosen: str, rejected: str) -> bool:
    p, c, rj = (prompt or "").strip(), (chosen or "").strip(), (rejected or "").strip()
    if not p or not c or not rj:
        return False
    if c.casefold() in INVALID_CHOSEN:
        return False
    if _norm(c) == _norm(rj):                      # no signal if identical
        return False
    if len(c) > MAX_CHOSEN_CHARS or len(p) > MAX_PROMPT_CHARS or len(rj) > MAX_REJECTED_CHARS:
        return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=os.path.join(TRAIN_DATA, "orpo_deliberation"),
                    help="output dir for train.jsonl / valid.jsonl")
    ap.add_argument("--valid-frac", type=float, default=0.1)
    ap.add_argument("--dry-run", action="store_true", help="print stats, write nothing")
    args = ap.parse_args()

    seen = set()
    pairs = []
    per_source = {}
    skipped = {"invalid": 0, "dup": 0}

    for pattern, pk, ck, rk in SOURCES:
        for path in sorted(glob.glob(pattern)):
            name = os.path.basename(path)
            n_ok = 0
            with open(path) as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        d = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    prompt, chosen, rejected = d.get(pk, ""), d.get(ck, ""), d.get(rk, "")
                    if not _valid(prompt, chosen, rejected):
                        skipped["invalid"] += 1
                        continue
                    key = (_norm(prompt), _norm(chosen), _norm(rejected))
                    if key in seen:
                        skipped["dup"] += 1
                        continue
                    seen.add(key)
                    pairs.append({
                        "prompt": prompt.strip(),
                        "chosen": chosen.strip(),
                        "rejected": rejected.strip(),
                        "_delib": _is_deliberation(rejected),
                    })
                    n_ok += 1
            per_source[name] = per_source.get(name, 0) + n_ok

    if not pairs:
        print("ERROR: no usable preference pairs produced", file=sys.stderr)
        return 2

    n = len(pairs)
    n_delib = sum(1 for p in pairs if p["_delib"])
    clen = sorted(len(p["chosen"]) for p in pairs)
    rlen = sorted(len(p["rejected"]) for p in pairs)
    med = lambda a: a[len(a) // 2]

    print(f"\n{'='*60}\n  ORPO DELIBERATION SET\n{'='*60}")
    for name, c in sorted(per_source.items()):
        print(f"  {name:38s} {c:5d} pairs")
    print(f"  {'-'*52}")
    print(f"  TOTAL usable pairs:                    {n:5d}")
    print(f"  deliberation-style rejected:           {n_delib:5d} ({100*n_delib//n}%)")
    print(f"  skipped (invalid/empty/None chosen):   {skipped['invalid']:5d}")
    print(f"  skipped (duplicate):                   {skipped['dup']:5d}")
    print(f"  chosen len   median={med(clen)}  max={clen[-1]}")
    print(f"  rejected len median={med(rlen)}  max={rlen[-1]}")

    # Deterministic split (sorted by content hash proxy -> stable, no RNG).
    pairs.sort(key=lambda p: (p["prompt"], p["chosen"]))
    n_valid = max(1, int(n * args.valid_frac))
    # interleave pick for valid so both splits span all sources/lengths
    valid = pairs[::max(1, n // n_valid)][:n_valid]
    valid_keys = {id(p) for p in valid}
    train = [p for p in pairs if id(p) not in valid_keys]

    def _emit(records):
        return "\n".join(json.dumps({k: r[k] for k in ("prompt", "chosen", "rejected")})
                         for r in records) + "\n"

    print(f"\n  split: train={len(train)}  valid={len(valid)}")
    if args.dry_run:
        print("  (--dry-run: nothing written)")
        return 0

    os.makedirs(args.out, exist_ok=True)
    train_path = os.path.join(args.out, "train.jsonl")
    valid_path = os.path.join(args.out, "valid.jsonl")
    with open(train_path, "w") as f:
        f.write(_emit(train))
    with open(valid_path, "w") as f:
        f.write(_emit(valid))
    print(f"  wrote {train_path}")
    print(f"  wrote {valid_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
