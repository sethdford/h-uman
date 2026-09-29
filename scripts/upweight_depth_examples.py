#!/usr/bin/env python3
"""Upweight Seth's depth replies in a staged training corpus.

Why: fine-tuning on Seth's texts learns his TYPICAL message (median 27 chars in
seth-sft-20260919). The replies that carry his warmth and follow-through are a
minority by volume: long replies (p90 = 77 chars), questions asked back (12.6%),
answers to an emotional message (11.2%). E.g. "Why so down? How can I help?",
"How's your mood today?". Averaged in at weight 1, they get flattened, which is
the "dead-end replies, cold in hard moments" complaint (2026-09-24).

What: each row is emitted 1..MAX_WEIGHT times: +1 for a long reply, +1 for a
question asked back, +1 for replying to an emotional message. Output is
shuffled with a fixed seed so copies don't sit adjacent in a batch.

Where it runs: scripts/train-glm-adapter.sh, on the STAGED train.jsonl only,
after dedup and after the train/valid split. Upstream dedups would collapse the
copies; weighting before the split would leak copies into valid.jsonl and
inflate the "adapter learned" loss check. valid.jsonl is never touched.

Handles ORPO rows {prompt, chosen, rejected} (weights on `chosen`) and SFT rows
{prompt, completion}. Refuses (exit 2, writes nothing) on empty input.
"""
import argparse
import json
import os
import random
import re
import sys
import tempfile

MAX_WEIGHT = 3
# Floor on the "long reply" threshold: in a tiny corpus p90 can land on a
# 3-character reply and mark everything long.
MIN_LONG_CHARS = 60

# Word-bounded so "dismissed" doesn't read as "miss", or "hardware" as "hard".
_EMOTION = re.compile(
    r"\b(sad|miss(ing)?|hurt(ing)?|scared|afraid|anxious|worried|stress(ed|ful)?|"
    r"exhausted|sick|cry(ing)?|lonely|lost|rough|hard day|upset|depressed|"
    r"overwhelmed|angry|frustrated|heartbroken|grief|funeral|passed away|"
    r"excited|proud|nervous|love you)\b",
    re.IGNORECASE,
)


def reply_of(row):
    """The Seth-authored side of a row: ORPO `chosen` or SFT `completion`."""
    if "chosen" in row:
        return row["chosen"]
    if "completion" in row:
        return row["completion"]
    raise ValueError(f"unrecognized row shape (keys={sorted(row)})")


def weight_for(prompt, reply, long_threshold):
    """How many copies of this example to train on (1..MAX_WEIGHT)."""
    w = 1
    if len(reply) >= long_threshold:
        w += 1
    if "?" in reply:
        w += 1
    if _EMOTION.search(prompt or ""):
        w += 1
    return min(w, MAX_WEIGHT)


def _long_threshold(rows):
    lengths = sorted(len(reply_of(r)) for r in rows)
    p90 = lengths[int(0.9 * (len(lengths) - 1))]
    return max(p90, MIN_LONG_CHARS)


def upweight(rows, seed=1234):
    """Return (weighted_rows, stats). Raises ValueError on empty input."""
    if not rows:
        raise ValueError("empty corpus: nothing to weight")
    thr = _long_threshold(rows)
    out = []
    hist = {w: 0 for w in range(1, MAX_WEIGHT + 1)}
    for r in rows:
        w = weight_for(r.get("prompt", ""), reply_of(r), thr)
        hist[w] += 1
        out.extend([r] * w)
    random.Random(seed).shuffle(out)
    stats = {
        "rows_in": len(rows),
        "rows_out": len(out),
        "long_threshold_chars": thr,
        "weight_histogram": {str(k): v for k, v in hist.items()},
        "max_weight": MAX_WEIGHT,
        "seed": seed,
    }
    return out, stats


def _atomic_write(path, text):
    d = os.path.dirname(os.path.abspath(path)) or "."
    fd, tmp = tempfile.mkstemp(prefix=".upweight-", dir=d)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--input", required=True, help="staged train.jsonl")
    ap.add_argument("--output", required=True, help="weighted train.jsonl to write")
    ap.add_argument("--sidecar", required=True, help="stats JSON to write")
    ap.add_argument("--seed", type=int, default=1234)
    a = ap.parse_args(argv)

    with open(a.input, encoding="utf-8") as f:
        rows = [json.loads(line) for line in f if line.strip()]
    try:
        out, stats = upweight(rows, seed=a.seed)
    except ValueError as e:
        print(f"upweight: refusing: {e}", file=sys.stderr)
        return 2
    _atomic_write(a.output, "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in out))
    _atomic_write(a.sidecar, json.dumps(stats, indent=2) + "\n")
    print(f"upweight: {stats['rows_in']} -> {stats['rows_out']} rows "
          f"(long>={stats['long_threshold_chars']} chars, weights {stats['weight_histogram']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
