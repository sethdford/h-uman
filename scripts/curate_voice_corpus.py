#!/usr/bin/env python3
"""
Curate the harvested voice corpus toward the "ideal you" + format for mlx_lm LoRA.

"Best of me" is a better training target than "exact replica": rather than learning
the average (which includes one-word logistics and curt acks), we weight toward the
most substantive, lexically-rich, you-sounding messages — so the adapter leans
toward your best self while staying authentically your voice.

Scores each harvested message (length sweet-spot, lexical richness, thoughtfulness
markers, minus pure-logistics), keeps the top fraction, and writes mlx_lm chat-
format train/valid JSONL (90/10) ready for `mlx_lm.lora`.

Usage:
  scripts/curate_voice_corpus.py --in ~/.human/voice_corpus.jsonl \\
      --out-dir ~/.human/training-data/voice-ideal [--keep-frac 0.7]
"""

import argparse
import json
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import fidelity_axes as fx

SYSTEM = ("You are Seth. Reply in his exact texting voice — natural, concise, real "
          "opinions and warmth, lowercase-leaning, no AI-assistant tells.")
THOUGHTFUL = ("because", "think", "feel", "honestly", "appreciate", "remember", "wonder",
              "love", "proud", "sorry", "grateful", "excited", "imagine", "hope")
LOGISTICS = ("on my way", "omw", "be there", "running late", "address", "see you at")


def quality_score(text):
    """Higher = more 'ideal-you' exemplar. Heuristic, deterministic."""
    t = text.strip()
    n = len(t)
    f = fx.message_features(t)
    words = fx._words(t)
    # length sweet-spot: substantive but still texty (20-220 chars)
    if n < 12:
        length = 0.2
    elif n <= 220:
        length = 1.0
    else:
        length = max(0.3, 1.0 - (n - 220) / 600.0)
    # lexical richness within the message (per-message TTR)
    richness = (len(set(words)) / len(words)) if words else 0.0
    thoughtful = 1.0 if any(m in t.lower() for m in THOUGHTFUL) else 0.0
    logistics = 1.0 if any(m in t.lower() for m in LOGISTICS) else 0.0
    # weighted blend; thoughtfulness boosts, pure logistics penalizes
    return 0.45 * length + 0.30 * richness + 0.20 * thoughtful - 0.25 * logistics


def curate(messages, keep_frac):
    scored = sorted(((quality_score(m), m) for m in messages), key=lambda x: -x[0])
    keep = max(1, int(len(scored) * keep_frac))
    return [m for _, m in scored[:keep]]


def to_chat_record(msg):
    return {"messages": [
        {"role": "system", "content": SYSTEM},
        {"role": "user", "content": "reply in your voice"},
        {"role": "assistant", "content": msg},
    ]}


def main():
    ap = argparse.ArgumentParser(description="Curate voice corpus toward ideal-you + format for LoRA")
    ap.add_argument("--in", dest="inp", type=Path, default=Path.home() / ".human" / "voice_corpus.jsonl")
    ap.add_argument("--out-dir", type=Path, default=Path.home() / ".human" / "training-data" / "voice-ideal")
    ap.add_argument("--keep-frac", type=float, default=0.7)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    if not args.inp.exists():
        print(f"[error] corpus not found: {args.inp} (run harvest_imessage_voice.py first)", file=sys.stderr)
        return 1
    msgs = [json.loads(l)["text"] for l in args.inp.read_text().splitlines() if l.strip()]
    curated = curate(msgs, args.keep_frac)

    random.Random(args.seed).shuffle(curated)
    cut = max(1, int(len(curated) * 0.9))
    train, valid = curated[:cut], curated[cut:]

    args.out_dir.mkdir(parents=True, exist_ok=True)
    for name, rows in (("train", train), ("valid", valid)):
        with (args.out_dir / f"{name}.jsonl").open("w") as fh:
            for m in rows:
                fh.write(json.dumps(to_chat_record(m)) + "\n")

    print(json.dumps({
        "input_messages": len(msgs),
        "curated_kept": len(curated),
        "train": len(train),
        "valid": len(valid),
        "out_dir": str(args.out_dir),
        "format": "mlx_lm chat (messages[])",
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
