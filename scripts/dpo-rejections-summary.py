#!/usr/bin/env python3
"""
Summarize DPO negative-pair rejections captured by response_guard.

Reads daily-rotated JSONL files under ~/.human/training-data/ produced by
`hu_response_guard_log_dpo_negative` (Sprint 41 follow-up #3) and prints
operator-readable summaries: detector breakdown, channel breakdown, top
rejected drafts (signal that the LoRA is stuck in a specific pattern),
and a back-of-envelope retraining-signal-strength score.

Spec context: docs/plans/2026-05-26-m3-dispatch-unification/STATUS.md
lists this as the planned consumer for the DPO file. Without it, the
file is write-only — operators have to grep to see what's been rejected.
This turns the file into a debugging tool, which is the prerequisite
for any future "retrain the adapter on rejected drafts" work.

Schema (one JSON object per line):
  {"prompt": "...",
   "chosen": null,
   "rejected": "tbh morning. you awake yet?",
   "_source": "response_guard",
   "_detector": "naked_discourse_opener",
   "_channel": "imessage",
   "_ts_unix": 1779800000}

Usage:
  python3 scripts/dpo-rejections-summary.py [--days N] [--channel NAME]
                                            [--detector NAME] [--top K]
                                            [--root DIR]

Defaults:
  --days 7        # last week of rotated files
  --top 10        # top-K most-frequent rejected drafts
  --root ~/.human/training-data/
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from datetime import date, datetime, timedelta, timezone
from pathlib import Path

# Filename pattern matches hu_response_guard_dpo_path_for_day's output:
#   m3-dpo-rejections-YYYY-MM-DD.jsonl
FILENAME_RE = re.compile(r"^m3-dpo-rejections-(\d{4})-(\d{2})-(\d{2})\.jsonl$")


def discover_files(root: Path, days: int) -> list[Path]:
    """Return rotated DPO files from the last `days` days (UTC), oldest first.

    The C-side writer rotates by UTC midnight; we match that for parity.
    Files older than the cutoff are skipped but not deleted — the writer
    keeps them indefinitely (rotation, not retention).
    """
    if not root.exists():
        return []
    cutoff = (datetime.now(timezone.utc) - timedelta(days=days)).date()
    matched: list[tuple[date, Path]] = []
    for f in root.iterdir():
        m = FILENAME_RE.match(f.name)
        if not m:
            continue
        try:
            file_date = date(int(m.group(1)), int(m.group(2)), int(m.group(3)))
        except ValueError:
            continue
        if file_date >= cutoff:
            matched.append((file_date, f))
    matched.sort(key=lambda pair: pair[0])
    return [p for _, p in matched]


def load_rows(files: list[Path]) -> list[dict]:
    """Read all rows from all files. Silently skips malformed lines
    because the writer is append-only and a crash mid-write would
    leave a torn last line — we want the summary to keep working."""
    rows: list[dict] = []
    for f in files:
        try:
            with f.open("r", encoding="utf-8", errors="replace") as fp:
                for line_num, line in enumerate(fp, start=1):
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rows.append(json.loads(line))
                    except json.JSONDecodeError:
                        # Torn last line OR human-edited file with a typo.
                        # Single warning per file would be noisy; just count.
                        continue
        except OSError as e:
            print(f"WARN: failed to read {f}: {e}", file=sys.stderr)
    return rows


def filter_rows(rows: list[dict], channel: str | None, detector: str | None) -> list[dict]:
    out = rows
    if channel:
        out = [r for r in out if r.get("_channel") == channel]
    if detector:
        out = [r for r in out if r.get("_detector") == detector]
    return out


def histogram(rows: list[dict], key: str) -> list[tuple[str, int]]:
    """Return [(value, count), ...] sorted descending by count."""
    counter: Counter[str] = Counter()
    for r in rows:
        v = r.get(key)
        if v is None:
            v = "<null>"
        counter[str(v)] += 1
    return counter.most_common()


def top_drafts(rows: list[dict], k: int) -> list[tuple[str, int]]:
    """Find the top-K most-frequently-rejected draft strings. High
    counts indicate the LoRA is stuck producing the same bad pattern
    — strong signal for the next retraining iteration."""
    counter: Counter[str] = Counter()
    for r in rows:
        rejected = r.get("rejected")
        if not rejected:
            continue
        # Normalize whitespace so "tbh morning" and "tbh  morning" merge.
        normalized = " ".join(str(rejected).split())
        counter[normalized] += 1
    return counter.most_common(k)


def retraining_signal_strength(rows: list[dict]) -> tuple[str, str]:
    """Back-of-envelope: how strong is the DPO training signal?

    Returns (verdict, reasoning).

    Heuristics:
      - <50 rejections total → WEAK; not enough data, wait for more.
      - rejections spread across many detectors evenly → DIFFUSE; the
        adapter has multiple unrelated failure modes; broad retrain.
      - >40% of rejections concentrated in ONE detector → FOCUSED;
        retraining will likely improve that detector's class hard.
      - top-1 rejected draft accounts for >10% of total → STUCK; the
        adapter is deterministically emitting the same bad text;
        DPO will fix it fast.
    """
    n = len(rows)
    if n < 50:
        return ("WEAK", f"only {n} rejections; need more data before retraining.")

    detector_hist = histogram(rows, "_detector")
    top_det, top_det_count = detector_hist[0] if detector_hist else ("<none>", 0)
    top_det_share = top_det_count / n if n else 0.0

    drafts = top_drafts(rows, 1)
    top_draft, top_draft_count = drafts[0] if drafts else ("", 0)
    top_draft_share = top_draft_count / n if n else 0.0

    if top_draft_share > 0.10:
        return (
            "STUCK",
            f"top rejected draft accounts for {top_draft_share*100:.1f}% of all "
            f"rejections ({top_draft_count}/{n}); LoRA is deterministically emitting "
            f"the same bad pattern. DPO retraining will fix this fast.",
        )
    if top_det_share > 0.40:
        return (
            "FOCUSED",
            f"{top_det_share*100:.1f}% of rejections in detector `{top_det}` "
            f"({top_det_count}/{n}); retraining will improve this detector class hard.",
        )
    return (
        "DIFFUSE",
        f"rejections spread across {len(detector_hist)} detectors; the adapter has "
        "multiple unrelated failure modes — broad retrain or per-detector LoRAs.",
    )


def print_summary(rows: list[dict], top_k: int) -> None:
    n = len(rows)
    print(f"\n=== DPO rejections summary ({n} total) ===\n")
    if n == 0:
        print("No rejections in the window. Either:")
        print("  - The unified path is healthy (good news).")
        print("  - The daemon hasn't been restarted to pick up the M3 work.")
        print("  - The DPO logger is misconfigured (check $HOME).")
        return

    # Time window covered.
    timestamps = [r.get("_ts_unix") for r in rows if isinstance(r.get("_ts_unix"), (int, float))]
    if timestamps:
        oldest = datetime.fromtimestamp(min(timestamps), tz=timezone.utc)
        newest = datetime.fromtimestamp(max(timestamps), tz=timezone.utc)
        print(f"Window: {oldest.isoformat()} → {newest.isoformat()} UTC")

    # Per-detector breakdown.
    print("\nBy detector:")
    for det, count in histogram(rows, "_detector"):
        pct = 100.0 * count / n
        print(f"  {det:<28} {count:>6}  ({pct:5.1f}%)")

    # Per-channel breakdown.
    print("\nBy channel:")
    for ch, count in histogram(rows, "_channel"):
        pct = 100.0 * count / n
        print(f"  {ch:<28} {count:>6}  ({pct:5.1f}%)")

    # Cross-tab: which detectors fire most on which channels.
    cross: defaultdict[tuple[str, str], int] = defaultdict(int)
    for r in rows:
        cross[(str(r.get("_channel") or "<null>"), str(r.get("_detector") or "<null>"))] += 1
    if cross:
        print("\nChannel × Detector (top 10):")
        for (ch, det), count in sorted(cross.items(), key=lambda kv: -kv[1])[:10]:
            print(f"  {ch:<16} {det:<28} {count:>6}")

    # Top rejected drafts — operator insight into "what's the model
    # actually trying to send".
    print(f"\nTop {top_k} most-rejected drafts:")
    for draft, count in top_drafts(rows, top_k):
        # Truncate long drafts to keep the table readable.
        preview = draft if len(draft) <= 80 else draft[:77] + "..."
        print(f"  [{count:>4}x] {preview}")

    # Retraining signal.
    verdict, reasoning = retraining_signal_strength(rows)
    print(f"\nRetraining signal: {verdict}")
    print(f"  {reasoning}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Summarize DPO negative-pair rejections from response_guard."
    )
    ap.add_argument(
        "--days",
        type=int,
        default=7,
        help="How many days of rotated files to include (default 7).",
    )
    ap.add_argument(
        "--channel",
        type=str,
        default=None,
        help="Filter to rows from this channel (e.g. imessage, voice).",
    )
    ap.add_argument(
        "--detector",
        type=str,
        default=None,
        help="Filter to rows from this detector (e.g. naked_discourse_opener).",
    )
    ap.add_argument(
        "--top", type=int, default=10, help="Show top-K most-rejected drafts (default 10)."
    )
    ap.add_argument(
        "--root",
        type=Path,
        default=Path.home() / ".human" / "training-data",
        help="Directory containing m3-dpo-rejections-YYYY-MM-DD.jsonl files.",
    )
    args = ap.parse_args()

    files = discover_files(args.root, args.days)
    if not files:
        print(f"No DPO rejection files found in {args.root} (last {args.days} days).")
        print("Either the daemon hasn't run yet, or the unified path hasn't rejected anything.")
        return 0

    print(f"Reading {len(files)} file(s) from {args.root}:")
    for f in files:
        print(f"  {f.name}")

    rows = load_rows(files)
    rows = filter_rows(rows, args.channel, args.detector)
    if args.channel or args.detector:
        filters = []
        if args.channel:
            filters.append(f"channel={args.channel}")
        if args.detector:
            filters.append(f"detector={args.detector}")
        print(f"\nFiltered: {', '.join(filters)} → {len(rows)} rows.")

    print_summary(rows, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
