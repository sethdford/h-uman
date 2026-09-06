#!/usr/bin/env python3
"""measure_emotion_card.py — derive the persona's EMOTION card from a window
of the user's own outbound iMessages and write it where the daemon reads it.

The style card (scripts/measure_style_card.py) measures punctuation, casing
and emoji. This measures the emotional register: which Cowen-Keltner
category each of the user's texts expresses, how strongly, and how often
a text is simply neutral. The daemon renders casual rule 14 from it when
HU_EMOTION_REGISTER is live (src/persona/emotion_card.c); the nightly
scripts/eval_emotion_register.py compares the twin's sent replies against
it with the same judge.

Judge: the LOCAL model on :8741 (scripts/emotion_register.LocalJudge). No
message text leaves this machine. The judge's identity is written into the
card so a later comparison by a different judge is refused.

Same read-only chat.db contract as the style card (mode=ro + immutable=1,
shared reader eval_persona_evolution.fetch_outbound_messages).

Refusal contract (.claude/rules/no-number-without-a-measurement.md):
  exit 2  judge unreachable — deferred, nothing written
  exit 3  window below --min-n, or parse failures above --max-parse-failure-rate
          — refused, nothing written

Usage:
    python3 scripts/measure_emotion_card.py                 # last 60 days, seth
    python3 scripts/measure_emotion_card.py --max-n 300     # cap the judged sample
    python3 scripts/measure_emotion_card.py --dry-run       # print, don't write
"""
import argparse
import datetime
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emotion_register import (  # noqa: E402
    DEFAULT_MLX_URL,
    PROMPT_SHA,
    TAXONOMY_VERSION,
    JudgeUnavailable,
    LocalJudge,
    MeasurementRefused,
    aggregate,
    label_texts,
)

SCHEMA = "emotion-card/v1"
DEFAULT_DAYS = 60
DEFAULT_MIN_N = 100
DEFAULT_MAX_N = 300
DEFAULT_MAX_PARSE_FAILURE_RATE = 0.10
DEFAULT_PERSONA = "seth"
DEFAULT_DB = os.path.expanduser("~/Library/Messages/chat.db")


def default_card_path(persona: str) -> str:
    base = os.environ.get("HU_PERSONA_DIR") or os.path.expanduser("~/.human/personas")
    return os.path.join(base, f"{persona}.emotion-card.json")


def select_window(messages, window_start, window_end, max_n: int, seed: int):
    """Keep (ts, text) pairs inside [start, end); sample max_n of them
    deterministically when the window is larger. Returns (kept, n_window)."""
    inside = [(ts, text) for ts, text in messages if window_start <= ts < window_end]
    n_window = len(inside)
    if n_window > max_n:
        rng = random.Random(seed)
        inside = rng.sample(inside, max_n)
        inside.sort()
    return inside, n_window


def build_card(labels, persona: str, window_start, window_end, n_window: int,
               judge_meta: dict, min_n: int = DEFAULT_MIN_N,
               max_parse_failure_rate: float = DEFAULT_MAX_PARSE_FAILURE_RATE,
               n_resamples: int = 2000, seed: int = 42) -> dict:
    """Aggregate labels into an emotion-card/v1 dict, or refuse."""
    if not labels:
        raise MeasurementRefused("no messages to label")
    failure_rate = sum(1 for lab in labels if lab is None) / len(labels)
    if failure_rate > max_parse_failure_rate:
        raise MeasurementRefused(
            f"judge replies failed to parse for {failure_rate:.0%} of messages "
            f"(> {max_parse_failure_rate:.0%}); the judge is not labeling this corpus"
        )
    agg = aggregate(labels, n_resamples=n_resamples, seed=seed)
    if agg["n"] < min_n:
        raise MeasurementRefused(
            f"window {window_start.date()}..{window_end.date()} has n={agg['n']} labeled "
            f"< min_n={min_n}"
        )
    return {
        "schema": SCHEMA,
        "persona": persona,
        "source": "scripts/measure_emotion_card.py",
        "generated_at": datetime.datetime.now().replace(microsecond=0).isoformat(),
        "taxonomy": TAXONOMY_VERSION,
        "judge": judge_meta,
        "window": {
            "start": window_start.date().isoformat(),
            "end": window_end.date().isoformat(),
            "days": (window_end - window_start).days,
        },
        "n": agg["n"],
        "n_window": n_window,
        "n_sampled": len(labels),
        "parse_failures": agg["parse_failures"],
        "min_n": min_n,
        "max_parse_failure_rate": max_parse_failure_rate,
        "confidence": 0.95,
        "distribution": agg["distribution"],
        "neutral_share": agg["neutral_share"],
        "mean_intensity": agg["mean_intensity"],
        "valence_mean": agg["valence_mean"],
        "top": agg["top"],
    }


def write_card(card: dict, path: str) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = f"{path}.tmp-{os.getpid()}"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(card, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, path)


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", default=DEFAULT_DB)
    p.add_argument("--persona", default=DEFAULT_PERSONA)
    p.add_argument("--days", type=int, default=DEFAULT_DAYS)
    p.add_argument("--end", default=None, help="window end, YYYY-MM-DD (default: today)")
    p.add_argument("--min-n", type=int, default=DEFAULT_MIN_N,
                   help="refuse to write a card below this many labeled messages")
    p.add_argument("--max-n", type=int, default=DEFAULT_MAX_N,
                   help="judge at most this many messages (deterministic sample)")
    p.add_argument("--max-parse-failure-rate", type=float,
                   default=DEFAULT_MAX_PARSE_FAILURE_RATE)
    p.add_argument("--mlx-url", default=os.environ.get("HUMAN_MLX_URL", DEFAULT_MLX_URL))
    p.add_argument("--n-resamples", type=int, default=2000)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--out", default=None,
                   help="card path (default: $HU_PERSONA_DIR or ~/.human/personas/<persona>.emotion-card.json)")
    p.add_argument("--dry-run", action="store_true", help="print the card, write nothing")
    return p.parse_args(argv)


def _window(args):
    end = (datetime.datetime.strptime(args.end, "%Y-%m-%d") if args.end
           else datetime.datetime.now())
    return end - datetime.timedelta(days=args.days), end


def run(args, messages=None, judge=None) -> int:
    start, end = _window(args)
    if messages is None:
        from eval_persona_evolution import fetch_outbound_messages  # noqa: E402
        messages = fetch_outbound_messages(args.db, start, end)
    sample, n_window = select_window(messages, start, end, args.max_n, args.seed)
    if len(sample) < args.min_n:
        sys.stderr.write(
            f"REFUSED: window {start.date()}..{end.date()} has n={len(sample)} "
            f"< min_n={args.min_n}; wrote nothing.\n")
        return 3
    judge = judge or LocalJudge(args.mlx_url)
    try:
        model = judge.model()
    except JudgeUnavailable as e:
        sys.stderr.write(f"DEFERRED: judge unreachable ({e}); wrote nothing.\n")
        return 2
    judge_meta = {"id": judge.id(), "model": model, "taxonomy": TAXONOMY_VERSION,
                  "prompt_sha": PROMPT_SHA, "url": getattr(judge, "base_url", "")}
    sys.stderr.write(f"labeling {len(sample)} of {n_window} messages with {model}\n")
    try:
        labels = label_texts(judge, [t for _, t in sample],
                             progress=lambda i, n: sys.stderr.write(f"  {i}/{n}\n"))
        card = build_card(labels, args.persona, start, end, n_window, judge_meta,
                          min_n=args.min_n,
                          max_parse_failure_rate=args.max_parse_failure_rate,
                          n_resamples=args.n_resamples, seed=args.seed)
    except JudgeUnavailable as e:
        sys.stderr.write(f"DEFERRED: judge failed mid-run ({e}); wrote nothing.\n")
        return 2
    except MeasurementRefused as e:
        sys.stderr.write(f"REFUSED: {e}; wrote nothing.\n")
        return 3
    if args.dry_run:
        print(json.dumps(card, indent=2, ensure_ascii=False))
        return 0
    path = args.out or default_card_path(args.persona)
    write_card(card, path)
    top = ", ".join(f"{t['emotion']} {t['share']:.0%}" for t in card["top"]) or "none"
    print(f"wrote {path}: n={card['n']} neutral={card['neutral_share']['value']:.0%} "
          f"intensity={card['mean_intensity']['value']:.2f} top: {top}")
    return 0


def main(argv=None) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
