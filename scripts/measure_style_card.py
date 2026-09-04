#!/usr/bin/env python3
"""measure_style_card.py — derive the persona's style card from a window of
the user's own outbound iMessages and write it where the daemon reads it.

Why this exists (2026-09-03): the persona's style numbers contradicted each
other AND the measurement — lowercase-start was 4% in a C comment, 17.3% in
the old style card, 8.6% measured; emoji was "1 in 8" in seth.json, 9% in
the card, 12.6% measured. The July 2026 deliberation leak was traced to
exactly this shape: three prompt layers asserting different numbers for
the same axis, and the model agonizing over the conflict on every turn.

This script makes the card the single source. The C prompt builder
(src/persona/style_card.c) renders the casual register's rule 2 from
~/.human/personas/<persona>.style-card.json; a compiled default is only a
fallback when the card is missing (and it logs that it fell back).

Axes are computed by scripts/eval_persona_evolution.py's per-message
feature functions, so the card and the gap-analysis eval can never
disagree on a definition. Same read-only chat.db contract as that script:
mode=ro + immutable=1, no message text ever leaves this process.

Refusal contract (.claude/rules/no-number-without-a-measurement.md): if the
window has fewer than --min-n messages (default 300) the script exits
non-zero and writes NOTHING. A stale card is recoverable; a card derived
from 40 messages is not distinguishable from a real one downstream.

Usage:
    python3 scripts/measure_style_card.py                  # last 60 days, seth
    python3 scripts/measure_style_card.py --days 90 --persona seth
    python3 scripts/measure_style_card.py --dry-run        # print, don't write

Supersedes the card-writing half of scripts/persona_style_card.py (whose
v1 output shape nothing in C ever read).
"""
import argparse
import datetime
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_persona_evolution import (  # noqa: E402
    DEFAULT_DB,
    aggregate_window,
    fetch_outbound_messages,
)

SCHEMA = "style-card/v2"
DEFAULT_DAYS = 60
DEFAULT_MIN_N = 300
DEFAULT_PERSONA = "seth"

# Axes the C renderer consumes (include/human/persona/style_card.h) plus
# length for the record. Names are eval_persona_evolution.AXES labels.
CARD_AXES = (
    "lowercase_start_rate",
    "no_terminal_punct_rate",
    "question_rate",
    "exclamation_rate",
    "emoji_rate",
    "length_chars",
)


class InsufficientData(Exception):
    """Raised instead of producing a card from too few messages."""


def default_card_path(persona: str) -> str:
    base = os.environ.get("HU_PERSONA_DIR") or os.path.expanduser("~/.human/personas")
    return os.path.join(base, f"{persona}.style-card.json")


def build_card(messages, persona: str, window_start: datetime.datetime,
               window_end: datetime.datetime, min_n: int = DEFAULT_MIN_N,
               n_resamples: int = 2000, seed: int = 42) -> dict:
    """messages: iterable of (datetime, text). Pure — no I/O.

    Keeps only messages with window_start <= ts < window_end, refuses below
    min_n, and returns the card dict. Every axis carries value + 95% CI + n
    so a reader can tell a tight estimate from a noisy one.
    """
    texts = [t for ts, t in messages if window_start <= ts < window_end]
    n = len(texts)
    if n < min_n:
        raise InsufficientData(
            f"window {window_start.date()}..{window_end.date()} has n={n} < min_n={min_n}"
        )
    agg = aggregate_window(texts, n_resamples=n_resamples, seed=seed)
    axes = {}
    for name in CARD_AXES:
        a = agg["axes"][name]
        axes[name] = {
            "value": a["mean"],
            "ci_lo": a["ci_lo"],
            "ci_hi": a["ci_hi"],
            "n": a["n"],
        }
    return {
        "schema": SCHEMA,
        "persona": persona,
        "source": "scripts/measure_style_card.py",
        "generated_at": datetime.datetime.now().replace(microsecond=0).isoformat(),
        "window": {
            "start": window_start.date().isoformat(),
            "end": window_end.date().isoformat(),
            "days": (window_end - window_start).days,
        },
        "n": n,
        "min_n": min_n,
        "confidence": 0.95,
        "axes": axes,
    }


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", default=DEFAULT_DB)
    p.add_argument("--persona", default=DEFAULT_PERSONA)
    p.add_argument("--days", type=int, default=DEFAULT_DAYS,
                   help="window length ending at --end (default: %(default)s)")
    p.add_argument("--end", default=None, help="window end, YYYY-MM-DD (default: today)")
    p.add_argument("--min-n", type=int, default=DEFAULT_MIN_N,
                   help="refuse to write a card below this many messages (default: %(default)s)")
    p.add_argument("--n-resamples", type=int, default=2000)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--out", default=None,
                   help="card path (default: $HU_PERSONA_DIR or ~/.human/personas/<persona>.style-card.json)")
    p.add_argument("--dry-run", action="store_true", help="print the card, write nothing")
    return p.parse_args(argv)


def run(args, messages=None) -> int:
    end = (datetime.datetime.strptime(args.end, "%Y-%m-%d")
           if args.end else datetime.datetime.now())
    start = end - datetime.timedelta(days=args.days)
    if messages is None:
        messages = fetch_outbound_messages(args.db, start, end)
    try:
        card = build_card(messages, args.persona, start, end, min_n=args.min_n,
                          n_resamples=args.n_resamples, seed=args.seed)
    except InsufficientData as e:
        sys.stderr.write(f"REFUSED: {e}; wrote nothing.\n")
        return 1

    print(json.dumps(card, indent=2))
    if args.dry_run:
        return 0
    out = args.out or default_card_path(args.persona)
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    tmp = out + ".tmp"
    Path(tmp).write_text(json.dumps(card, indent=2) + "\n")
    os.replace(tmp, out)
    sys.stderr.write(f"wrote {out} (n={card['n']}, window={card['window']['days']}d)\n")
    return 0


def main(argv=None) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
