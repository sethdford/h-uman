#!/usr/bin/env python3
"""eval_emotion_register.py — nightly: does the twin FEEL like the user?

Labels the twin's recently SENT iMessage replies (memory.db
production_outcomes — real deliveries, not harness output) with the same
local judge that built the persona's emotion card, and reports the
Jensen-Shannon divergence between the two category distributions plus
deltas on neutral share, mean intensity and valence.

This is a measurement, not a gate: the verdict is MEASURED with a
provisional `gap` flag (jsd > --jsd-max). It is stage [4/4] of
scripts/nightly_eval.sh and feeds the decision of whether/how to flip
HU_EMOTION_REGISTER from off (feature-gate-requires-measurement.md).

Refusal contract (.claude/rules/no-number-without-a-measurement.md):
  exit 0  measured; verdict JSON written
  exit 2  judge unreachable — deferred, nothing written
  exit 3  no card / judge mismatch with the card / fewer than --min-n
          replies / parse failures above the rate — refused, nothing written

Usage:
    python3 scripts/eval_emotion_register.py                       # last 14 days
    python3 scripts/eval_emotion_register.py --output-json out.json
"""
import argparse
import datetime
import json
import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emotion_register import (  # noqa: E402
    DEFAULT_MLX_URL,
    PROMPT_SHA,
    TAXONOMY_VERSION,
    JudgeUnavailable,
    LocalJudge,
    MeasurementRefused,
    check_judge_match,
    compare,
    label_texts,
)
from measure_emotion_card import SCHEMA as CARD_SCHEMA  # noqa: E402
from measure_emotion_card import default_card_path  # noqa: E402

SCHEMA = "emotion-register/v1"
DEFAULT_DAYS = 14
DEFAULT_MIN_N = 20
DEFAULT_MAX_N = 200
DEFAULT_JSD_MAX = 0.15
DEFAULT_MAX_PARSE_FAILURE_RATE = 0.10
DEFAULT_MEMORY_DB = os.path.expanduser("~/.human/memory.db")
DEFAULT_OUTPUT = os.path.expanduser("~/.human/logs/eval-emotion-register-latest.json")


def fetch_twin_replies(memory_db: str, since_ts: int, max_n: int):
    """Newest-first (send_timestamp, chosen) rows the daemon actually sent on
    iMessage since `since_ts`. Read-only open; the daemon owns this DB."""
    uri = f"file:{memory_db}?mode=ro"
    con = sqlite3.connect(uri, uri=True)
    try:
        rows = con.execute(
            """
            SELECT send_timestamp, chosen FROM production_outcomes
            WHERE channel = 'imessage' AND length(chosen) > 0 AND send_timestamp >= ?
            ORDER BY send_timestamp DESC LIMIT ?
            """,
            (int(since_ts), int(max_n)),
        ).fetchall()
    finally:
        con.close()
    return [(int(ts), str(text).strip()) for ts, text in rows if str(text).strip()]


def load_card(path: str) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            card = json.load(f)
    except (OSError, ValueError) as e:
        raise MeasurementRefused(f"emotion card unreadable at {path}: {e}") from e
    if card.get("schema") != CARD_SCHEMA:
        raise MeasurementRefused(f"{path} is not {CARD_SCHEMA} (schema={card.get('schema')!r})")
    for key in ("distribution", "judge", "n", "neutral_share", "mean_intensity", "valence_mean"):
        if key not in card:
            raise MeasurementRefused(f"{path} lacks '{key}'")
    return card


def build_verdict(card: dict, labels, judge_meta: dict, window: dict,
                  jsd_max: float = DEFAULT_JSD_MAX,
                  max_parse_failure_rate: float = DEFAULT_MAX_PARSE_FAILURE_RATE,
                  min_n: int = DEFAULT_MIN_N, n_resamples: int = 2000,
                  seed: int = 42) -> dict:
    if not labels:
        raise MeasurementRefused("no twin replies to label")
    failure_rate = sum(1 for lab in labels if lab is None) / len(labels)
    if failure_rate > max_parse_failure_rate:
        raise MeasurementRefused(
            f"judge replies failed to parse for {failure_rate:.0%} of replies "
            f"(> {max_parse_failure_rate:.0%})")
    result = compare(card, labels, n_resamples=n_resamples, seed=seed)
    if result["jsd"]["n"] < min_n:
        raise MeasurementRefused(f"only {result['jsd']['n']} labeled replies < min_n={min_n}")
    return {
        "schema": SCHEMA,
        "source": "scripts/eval_emotion_register.py",
        "generated_at": datetime.datetime.now().replace(microsecond=0).isoformat(),
        "verdict": "MEASURED",
        "gap": result["jsd"]["value"] > jsd_max,
        "jsd_max": jsd_max,
        "jsd_max_note": "provisional threshold; recalibrate after three nightly verdicts",
        "judge": judge_meta,
        "card": {
            "persona": card.get("persona"),
            "generated_at": card.get("generated_at"),
            "n": card.get("n"),
            "window": card.get("window"),
            "neutral_share": card["neutral_share"],
            "mean_intensity": card["mean_intensity"],
            "valence_mean": card["valence_mean"],
            "top": card.get("top", []),
        },
        "window": window,
        "n": result["jsd"]["n"],
        "n_sampled": len(labels),
        "parse_failures": sum(1 for lab in labels if lab is None),
        "jsd": result["jsd"],
        "deltas": result["deltas"],
        "twin": {
            "neutral_share": result["twin"]["neutral_share"],
            "mean_intensity": result["twin"]["mean_intensity"],
            "valence_mean": result["twin"]["valence_mean"],
            "top": result["twin"]["top"],
            "distribution": result["twin"]["distribution"],
        },
        "largest_shifts": result["largest_shifts"],
    }


def write_verdict(verdict: dict, path: str) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = f"{path}.tmp-{os.getpid()}"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(verdict, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, path)


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--memory-db", default=DEFAULT_MEMORY_DB)
    p.add_argument("--persona", default="seth")
    p.add_argument("--card", default=None, help="emotion card path (default: the persona's)")
    p.add_argument("--days", type=int, default=DEFAULT_DAYS)
    p.add_argument("--min-n", type=int, default=DEFAULT_MIN_N)
    p.add_argument("--max-n", type=int, default=DEFAULT_MAX_N)
    p.add_argument("--jsd-max", type=float, default=DEFAULT_JSD_MAX)
    p.add_argument("--max-parse-failure-rate", type=float,
                   default=DEFAULT_MAX_PARSE_FAILURE_RATE)
    p.add_argument("--mlx-url", default=os.environ.get("HUMAN_MLX_URL", DEFAULT_MLX_URL))
    p.add_argument("--n-resamples", type=int, default=2000)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--output-json", default=DEFAULT_OUTPUT)
    p.add_argument("--dry-run", action="store_true", help="print the verdict, write nothing")
    return p.parse_args(argv)


def run(args, replies=None, judge=None, now=None) -> int:
    now = now or datetime.datetime.now()
    try:
        card = load_card(args.card or default_card_path(args.persona))
    except MeasurementRefused as e:
        sys.stderr.write(f"REFUSED: {e}; wrote nothing.\n")
        return 3
    judge = judge or LocalJudge(args.mlx_url)
    try:
        model = judge.model()
    except JudgeUnavailable as e:
        sys.stderr.write(f"DEFERRED: judge unreachable ({e}); wrote nothing.\n")
        return 2
    judge_meta = {"id": judge.id(), "model": model, "taxonomy": TAXONOMY_VERSION,
                  "prompt_sha": PROMPT_SHA, "url": getattr(judge, "base_url", "")}
    try:
        check_judge_match(card, judge_meta["id"])
    except MeasurementRefused as e:
        sys.stderr.write(f"REFUSED: {e}; wrote nothing.\n")
        return 3
    since = now - datetime.timedelta(days=args.days)
    if replies is None:
        replies = fetch_twin_replies(args.memory_db, int(since.timestamp()), args.max_n)
    window = {"start": since.date().isoformat(), "end": now.date().isoformat(),
              "days": args.days, "source": "production_outcomes(channel=imessage)"}
    if len(replies) < args.min_n:
        sys.stderr.write(
            f"REFUSED: only {len(replies)} sent replies in the last {args.days} days "
            f"< min_n={args.min_n}; wrote nothing.\n")
        return 3
    try:
        labels = label_texts(judge, [t for _, t in replies])
        verdict = build_verdict(card, labels, judge_meta, window, jsd_max=args.jsd_max,
                                max_parse_failure_rate=args.max_parse_failure_rate,
                                min_n=args.min_n, n_resamples=args.n_resamples,
                                seed=args.seed)
    except JudgeUnavailable as e:
        sys.stderr.write(f"DEFERRED: judge failed mid-run ({e}); wrote nothing.\n")
        return 2
    except MeasurementRefused as e:
        sys.stderr.write(f"REFUSED: {e}; wrote nothing.\n")
        return 3
    if args.dry_run:
        print(json.dumps(verdict, indent=2, ensure_ascii=False))
        return 0
    write_verdict(verdict, args.output_json)
    j = verdict["jsd"]
    d = verdict["deltas"]
    print(f"MEASURED n={verdict['n']} jsd={j['value']:.3f} [{j['ci_lo']:.3f}, {j['ci_hi']:.3f}] "
          f"gap={'yes' if verdict['gap'] else 'no'} d_neutral={d['neutral_share']:+.2f} "
          f"d_intensity={d['mean_intensity']:+.2f} d_valence={d['valence_mean']:+.2f} "
          f"-> {args.output_json}")
    return 0


def main(argv=None) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
