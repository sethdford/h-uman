#!/usr/bin/env python3
"""Convert the v6.1 preference corpus into chosen-only SFT data for mlx_lm.lora.

WHY THIS EXISTS: mlx-lm-lora 3.0.0's ORPO and CPO trainers silently produce
NO-OP adapters -- every lora_b stays exactly 0.0, so the LoRA term contributes
nothing and the result is bit-identical to the base model. Reproduced on a 2B
model in 60s: orpo 0/28 lora_b non-zero, cpo 0/28, dpo 28/28. DPO works but
always loads a second full copy of the model (load_reference_model), i.e. 2x56 GB
for GLM-4.5-Air, which does not fit in 128 GB.

So the objective that fits is broken and the objective that works does not fit.
The fallback is chosen-only SFT through `mlx_lm.lora` -- the trainer that
demonstrably works, since it produced the live seth-glm-air-v5 adapter.

WHAT IS LOST: SFT rewards the chosen reply but does not explicitly penalise the
rejected one, so it teaches "sound like this" rather than "and not like that".
The rejected side of each pair is discarded. That is a real weakening of the
objective and should be stated in any report of the resulting adapter.

WHAT IS KEPT: the n=40 re-weighting. The 6 human-detected pairs appear 8x each,
so their chosen replies are oversampled in the SFT set exactly as intended.

Output: {"prompt": ..., "completion": ...} rows, which mlx_lm's CompletionsDataset
reads. Pair with `mask_prompt: true` so loss is computed on the completion only.
"""
import argparse
import json
import os
import sys
from pathlib import Path

HOME = Path(os.path.expanduser("~"))


def convert(src_dir, out_dir, min_chars):
    src, out = Path(src_dir), Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    stats = {}
    for split in ("train", "valid"):
        rows, dropped = [], 0
        for line in open(src / f"{split}.jsonl"):
            d = json.loads(line)
            prompt, chosen = (d.get("prompt") or "").strip(), (d.get("chosen") or "").strip()
            if not prompt or not chosen:
                dropped += 1
                continue
            # A completion shorter than this is not a teachable reply -- it is
            # noise that biases the model toward one-word answers.
            if len(chosen) < min_chars:
                dropped += 1
                continue
            rows.append({"prompt": prompt, "completion": chosen})
        with open(out / f"{split}.jsonl", "w") as fh:
            for r in rows:
                fh.write(json.dumps(r, ensure_ascii=False) + "\n")
        stats[split] = (len(rows), dropped)
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=str(HOME / ".human/training-data/glm-v61-pref"))
    ap.add_argument("--out", default=str(HOME / ".human/training-data/glm-v62-sft"))
    ap.add_argument("--min-chars", type=int, default=2,
                    help="Drop completions shorter than this (default 2).")
    a = ap.parse_args()

    stats = convert(a.src, a.out, a.min_chars)
    total = sum(n for n, _ in stats.values())
    print(f"[sft-corpus] wrote {a.out}")
    for split, (n, dropped) in stats.items():
        print(f"  {split:6} {n:>4} rows  ({dropped} dropped)")
    if total < 200:
        sys.exit(f"FATAL: only {total} rows -- too thin to train on")

    lens = [len(json.loads(l)["completion"])
            for l in open(Path(a.out) / "train.jsonl")]
    lens.sort()
    print(f"  completion chars: p50={lens[len(lens)//2]} "
          f"p90={lens[int(.9*len(lens))]} max={lens[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
