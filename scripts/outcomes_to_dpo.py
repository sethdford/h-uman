#!/usr/bin/env python3
"""
AGI-C1c — outcomes_to_dpo: read resolved rows from production_outcomes,
generate dpo_pairs entries, mark the outcomes as processed.

This is where the production learning loop closes. After this job runs
nightly:

  outbound message → row in production_outcomes
                  → outcome (tapback / reply latency) arrives
                  → this job converts to dpo_pairs (chosen, rejected)
                  → next ORPO training round uses real production data
                  → trained adapter ships to inference path
                  → next outbound is shaped by yesterday's outcomes

See docs/plans/2026-05-19-agi-path.md.

Reinforcement-signal weighting (the policy this script implements):

  Positive tapback (polarity = +1) AND best-of-N alternatives present:
    For each alternative in `alternatives` JSON column:
      INSERT INTO dpo_pairs(prompt, chosen, rejected, margin, source)
      VALUES (prompt, chosen, alt, 0.7, 'outcome_tapback')

  Negative tapback (polarity = -1):
    A negative signal on chosen alone isn't a preference pair (no
    rejected alternative). Today we EXCLUDE these from dpo_pairs but
    log them for reflection (Capability 5). When best-of-N alternatives
    are present, we INVERT the pair: chosen becomes rejected, the
    top-P(Seth) alternative becomes chosen.

  No tapback but very fast reply (latency_s < 60):
    Soft positive signal. Generate a pair only when best-of-N alts
    exist; margin=0.4 (lower than explicit tapback).

  Slow reply (latency_s > 3600) on otherwise-OK message:
    Soft negative. Margin=0.3, source='outcome_slow_reply'.

The user can tune these weights — see the WEIGHTS dict below.

Usage:
  python3 scripts/outcomes_to_dpo.py                   # process all resolved rows
  python3 scripts/outcomes_to_dpo.py --dry-run         # show what would be written
  python3 scripts/outcomes_to_dpo.py --retrain-if N    # fire ml dpo-train if ≥N new pairs
"""

import argparse
import json
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

DB_DEFAULT = str(Path.home() / ".human" / "memory.db")

# Weighting policy — what reinforcement signals produce DPO pairs and
# at what margin. Tunable per deployment.
#
# margin is the DPO loss strength: higher = stronger signal that
# chosen > rejected. 0.8 is the existing reflection_retry margin;
# 0.7 for explicit tapback (slightly weaker because user might tap by
# accident); 0.4 for soft latency-based signal.
WEIGHTS = {
    "positive_tapback_margin": 0.70,
    "negative_tapback_inverted_margin": 0.65,  # only when alternatives exist
    "fast_reply_margin": 0.40,                  # latency_s < 60
    "fast_reply_threshold_s": 60,
    "slow_reply_margin": 0.30,
    "slow_reply_threshold_s": 3600,
}


def fetch_unprocessed_resolved(db_path: str, limit: int = 500) -> list:
    """Read rows where outcome_resolved_at IS NOT NULL AND processed_into_dpo = 0."""
    con = sqlite3.connect(db_path)
    con.row_factory = sqlite3.Row
    rows = con.execute(
        "SELECT id, channel, target, message_ref, prompt, chosen, "
        "alternatives, p_seth_at_send, send_timestamp, "
        "tapback_polarity, reply_latency_s, reply_length "
        "FROM production_outcomes "
        "WHERE outcome_resolved_at IS NOT NULL AND processed_into_dpo = 0 "
        "ORDER BY id LIMIT ?",
        (limit,),
    ).fetchall()
    con.close()
    return [dict(r) for r in rows]


def parse_alternatives(alt_json: str) -> list:
    """alternatives column stores a JSON array of best-of-N losers."""
    if not alt_json:
        return []
    try:
        v = json.loads(alt_json)
        return [a for a in v if isinstance(a, str) and len(a) >= 4]
    except (json.JSONDecodeError, TypeError):
        return []


def generate_pairs(row: dict) -> list:
    """Apply the weighting policy. Returns a list of dpo_pairs dicts."""
    pairs = []
    alts = parse_alternatives(row.get("alternatives"))
    polarity = row.get("tapback_polarity")
    latency = row.get("reply_latency_s")
    prompt = row["prompt"]
    chosen = row["chosen"]

    if polarity == 1:
        # Explicit positive tapback. Chosen wins against each alternative.
        for alt in alts:
            if alt == chosen or len(alt) < 4:
                continue
            pairs.append({
                "prompt": prompt, "chosen": chosen, "rejected": alt,
                "margin": WEIGHTS["positive_tapback_margin"],
                "source": "outcome_tapback",
            })
    elif polarity == -1 and alts:
        # Explicit negative. Invert the pair using top-P(Seth) alt as chosen.
        # NOTE: scoring alts with PersonaEval would require importing the
        # classifier; for now use the FIRST alternative as a reasonable
        # proxy (best-of-N's #2 candidate that didn't get sent). Future:
        # actually score alternatives.
        new_chosen = alts[0]
        if new_chosen != chosen and len(new_chosen) >= 4:
            pairs.append({
                "prompt": prompt, "chosen": new_chosen, "rejected": chosen,
                "margin": WEIGHTS["negative_tapback_inverted_margin"],
                "source": "outcome_tapback_inverted",
            })
    elif (polarity is None or polarity == 0) and alts and latency is not None:
        # Soft latency-based signal — no tapback, but they replied fast or
        # slow. Use as weaker positive/negative.
        if latency < WEIGHTS["fast_reply_threshold_s"]:
            # Fast reply = chosen landed well. Weak positive.
            for alt in alts[:1]:  # only use top alt to limit noise
                if alt != chosen and len(alt) >= 4:
                    pairs.append({
                        "prompt": prompt, "chosen": chosen, "rejected": alt,
                        "margin": WEIGHTS["fast_reply_margin"],
                        "source": "outcome_fast_reply",
                    })
        elif latency > WEIGHTS["slow_reply_threshold_s"]:
            # Slow reply = chosen may have stalled the conversation.
            # Treat top-alt as preferable; weak inversion.
            for alt in alts[:1]:
                if alt != chosen and len(alt) >= 4:
                    pairs.append({
                        "prompt": prompt, "chosen": alt, "rejected": chosen,
                        "margin": WEIGHTS["slow_reply_margin"],
                        "source": "outcome_slow_reply",
                    })
    return pairs


def write_pairs(db_path: str, pairs: list) -> int:
    if not pairs:
        return 0
    con = sqlite3.connect(db_path)
    now = int(time.time())
    written = 0
    for p in pairs:
        con.execute(
            "INSERT INTO dpo_pairs(prompt, chosen, rejected, margin, "
            "timestamp, source) VALUES (?, ?, ?, ?, ?, ?)",
            (p["prompt"], p["chosen"], p["rejected"], p["margin"],
             now, p["source"]),
        )
        written += 1
    con.commit()
    con.close()
    return written


def mark_processed(db_path: str, row_ids: list):
    if not row_ids:
        return
    con = sqlite3.connect(db_path)
    placeholders = ",".join("?" for _ in row_ids)
    con.execute(
        f"UPDATE production_outcomes SET processed_into_dpo = 1 "
        f"WHERE id IN ({placeholders})",
        row_ids,
    )
    con.commit()
    con.close()


def maybe_trigger_retrain(written: int, retrain_threshold: int) -> bool:
    """Fire `human ml dpo-train` if we wrote ≥ threshold pairs."""
    if written < retrain_threshold:
        return False
    # Find the human binary
    binary_candidates = [
        Path.home() / "bin" / "human",
        Path.home() / "Documents" / "h-uman" / "build" / "human",
    ]
    binary = next((p for p in binary_candidates if p.is_file() and p.exists()), None)
    if not binary:
        print("WARNING: human binary not found; skipping auto-retrain")
        return False
    print(f"  triggering: {binary} ml dpo-train --backend auto")
    try:
        r = subprocess.run(
            [str(binary), "ml", "dpo-train", "--backend", "auto"],
            capture_output=True, text=True, timeout=600,
        )
        ok = r.returncode == 0
        if ok:
            print("  retrain SUCCESS")
        else:
            print(f"  retrain FAILED (exit={r.returncode}): {r.stderr[-200:]}")
        return ok
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError) as e:
        print(f"  retrain ERROR: {e}")
        return False


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--db", default=DB_DEFAULT)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--limit", type=int, default=500)
    p.add_argument("--retrain-if", type=int, default=0,
                   help="Trigger `human ml dpo-train` if N or more pairs written")
    args = p.parse_args()

    if not Path(args.db).exists():
        print(f"DB not found: {args.db}", file=sys.stderr)
        sys.exit(2)

    rows = fetch_unprocessed_resolved(args.db, limit=args.limit)
    print(f"Fetched {len(rows)} unprocessed resolved outcomes from "
          f"{args.db}")
    if not rows:
        print("Nothing to do.")
        return

    all_pairs = []
    source_counts = {}
    for row in rows:
        pairs = generate_pairs(row)
        all_pairs.extend(pairs)
        for p in pairs:
            source_counts[p["source"]] = source_counts.get(p["source"], 0) + 1

    print(f"Generated {len(all_pairs)} pairs from {len(rows)} outcomes")
    for src, n in sorted(source_counts.items(), key=lambda kv: -kv[1]):
        print(f"  {src:<30} {n:>4}")

    if args.dry_run:
        print("\n--dry-run: not writing.")
        if all_pairs:
            print(f"Sample first pair:")
            sample = all_pairs[0]
            for k in ("source", "margin"):
                print(f"    {k}: {sample[k]}")
            for k in ("prompt", "chosen", "rejected"):
                print(f"    {k}: {sample[k][:80]!r}")
        return

    written = write_pairs(args.db, all_pairs)
    mark_processed(args.db, [r["id"] for r in rows])
    print(f"Wrote {written} dpo_pairs rows. Marked {len(rows)} outcomes "
          f"as processed_into_dpo=1.")

    if args.retrain_if > 0:
        maybe_trigger_retrain(written, args.retrain_if)


if __name__ == "__main__":
    main()
