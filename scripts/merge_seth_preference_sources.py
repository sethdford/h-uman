#!/usr/bin/env python3
"""merge_seth_preference_sources.py — merge the provenance-verified
Seth-authored preference sources named in
docs/plans/2026-09-02-persona-evolution/spec.md §3b into a single
KTO-shaped ({"prompt","completion","label"}) training corpus.

WHY THIS SCRIPT (sprint-better-than-human-2026-09-05 US-1)
------------------------------------------------------------
The 86%-lowercase-start regression traced (2026-09-04) to a CHOSEN side
that was 77.5% lowercase-start by construction (cycle-4/generated/arena
text, not Seth's own typing). scripts/rebalance_preference_corpus.py fixes
the CASING confound on an existing corpus, but doesn't grow the corpus's
Seth-authored CHOSEN pool. This script does that: it merges every
provenance-verified Seth-authored export (spec.md §3b "used"/"usable"
rows only -- never memory.db/dpo_pairs/production_outcomes, which are
daemon-authored) into one deduplicated KTO label=True pool, and pairs it
with an existing corpus's REJECTED side (label=False, unpaired by design
-- see "KTO, not paired DPO/ORPO" below) so the result can be handed to
--match-sides unmodified.

Sources (see designs/US-1.md §0 for the two corrections to stories.md's
AC-1.1, verified against the tree rather than assumed):
  --primary        data/imessage/training_pairs.jsonl (main checkout;
                    gitignored, not present in a worktree checkout)
  --extra (0+)      e.g. the 2026-07-25 eval-archive backups
                    (imessage-corpus-backup-20260725-113543/training_pairs.jsonl,
                    ground_truth-backup-20260725-113527.jsonl)
  --rejected-pool   an EXISTING preference corpus's {prompt,chosen,rejected}
                    or KTO train.jsonl -- only its `rejected` texts (with
                    their OWN original prompts) are read; provenance is not
                    required on this side (AC-1.2 only constrains chosen).

KTO shape, not paired DPO/ORPO -- why
--------------------------------------
None of the Seth-authored export shapes (training_pairs, ground_truth)
carry a competing ("rejected") reply for the same context -- producing one
would require a live model call, forbidden by AC-1.6 and
scripts/check-no-resident-model.sh. KTO training does not require
prompt correspondence between labels: each chosen row keeps its own real
prompt (the training_pairs shape's preceding context messages, or the
ground_truth shape's own `incoming` string); each reused rejected row
keeps its own real, different prompt. scripts/rebalance_preference_corpus.py
already accepts and exercises this shape end-to-end (including
--match-sides) with zero changes needed here.

Provenance admission (AC-1.2)
------------------------------
Every chosen-side row is shape-checked against the SAME two branches
scripts/eval_persona_evolution.py's _export_record_seth_text uses
(mirrored here, not imported -- that symbol is private (leading
underscore) and a sibling story this sprint, US-7, edits that file; this
keeps the admission check independent of that edit). A row that matches
neither shape is FATAL: refuse (exit non-zero), write nothing, name the
file and line. Daemon-authored stores (memory.db messages/dpo_pairs,
production_outcomes, m3-corpus.jsonl channel=memory_db rows) do not have
`messages`+`metadata.timestamp` or `seth_reply`+`timestamp` -- they
structurally cannot pass this check (verified against
docs/plans/2026-09-02-persona-evolution/spec.md:129-131's schemas), so
admission is structural, not a promise the caller has to keep.

De-dup
------
The exact (timestamp-to-the-second, sha256(stripped text)) key from
scripts/eval_persona_evolution.py:475-479 (dedup_key) is imported, not
re-derived -- per that module's own convention against a second hand-rolled
definition drifting (the 2026-07 deliberation-leak class of bug). Applied
across all chosen sources in order primary -> extra[0] -> extra[1] -> ...
(first-seen wins). merge_sources() itself only carries (timestamp, text)
2-tuples and would silently drop the `prompt` a KTO row needs, so the
merge loop here is a local variant carrying (timestamp, text, prompt)
3-tuples -- but it reuses the same imported dedup_key and reports the
same {"path","rows","added","duplicates"} stats shape merge_sources uses,
verbatim.

Privacy (AC-1.5)
-----------------
No message text is ever printed to stdout or written to manifest.json --
only counts, paths, and dates. train.jsonl (which DOES carry real message
text, by construction -- it is the training corpus) is written under
--out-dir, which must be outside the repo (this script does not enforce
the path is gitignored; the caller is responsible for pointing --out-dir
at ~/.human/training-data/ per AC-1.5).

Refuses (exit non-zero, writes nothing) when:
  - --primary, any --extra, or --rejected-pool path does not exist/is not
    readable;
  - any row in --primary or an --extra file fails the two-shape
    provenance check (FATAL, per-row, names file:line);
  - the merged, deduplicated chosen (label=True) pool has fewer than
    --floor rows (default 500);
  - --rejected-pool contains zero rows with a non-empty `rejected` field.

Usage:
    scripts/merge_seth_preference_sources.py \\
        --primary /Users/sethford/Projects/h-uman/data/imessage/training_pairs.jsonl \\
        --extra ~/.human/logs/eval-archive/imessage-corpus-backup-20260725-113543/training_pairs.jsonl \\
        --extra ~/.human/logs/eval-archive/ground_truth-backup-20260725-113527.jsonl \\
        --rejected-pool ~/.human/training-data/glm-v61-pref/train.jsonl \\
        --out-dir ~/.human/training-data/glm-v6-merged-20260905/
"""
import argparse
import datetime
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_persona_evolution import dedup_key, TAPBACK_PREFIXES  # noqa: E402

DEFAULT_FLOOR = 500


# ---------------------------------------------------------------------------
# Shape admission -- mirrors (does not import; private + touched by a
# sibling story this sprint) eval_persona_evolution.py:438-451's two
# accepted Seth-authored export shapes, extended to also return the row's
# own real prompt/context.
# ---------------------------------------------------------------------------


def extract_seth_record(rec):
    """Return (timestamp_iso_str, text, prompt) for one Seth-authored
    export row, or raise ValueError if the row carries no Seth-authorship
    provenance (this IS the AC-1.2 admission check).

    - training_pairs shape: {"messages":[...], "metadata":{"timestamp":...}}
      messages[-1] must be the assistant (Seth, is_from_me=1) turn.
      prompt = messages[:-1] verbatim (the real preceding context, not a
      synthesized string).
    - ground_truth shape: {"seth_reply":..., "timestamp":..., "incoming":...}
      prompt = the row's own `incoming` string.
    """
    if "messages" in rec and isinstance(rec.get("metadata"), dict) and "timestamp" in rec["metadata"]:
        msgs = rec["messages"]
        last = msgs[-1] if msgs else None
        if not isinstance(last, dict) or last.get("role") != "assistant":
            raise ValueError("training_pairs record whose last turn is not the assistant (is_from_me=1) turn")
        return rec["metadata"]["timestamp"], last.get("content"), msgs[:-1]
    if "seth_reply" in rec and "timestamp" in rec:
        return rec["timestamp"], rec["seth_reply"], rec.get("incoming")
    raise ValueError(
        "unrecognized export shape (need training_pairs messages/metadata.timestamp "
        "or ground_truth seth_reply/timestamp); DPO/memory.db rows carry no Seth provenance"
    )


def read_seth_source(path):
    """Read one Seth-authored export file end to end.

    Returns (rows, n_lines, n_dropped_empty, n_dropped_tapback) where
    `rows` is list[(datetime, text, prompt)] sorted by time. FATAL (raises
    SystemExit, nothing written anywhere) on the first row that is not
    valid JSON or fails extract_seth_record's shape check -- this is the
    per-row FATAL contract, not a skip.
    """
    rows = []
    n_lines = 0
    n_dropped_empty = 0
    n_dropped_tapback = 0
    with open(path) as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            n_lines += 1
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as e:
                raise SystemExit(f"REFUSING: {path}:{lineno} is not valid JSON ({e}); nothing written")
            if not isinstance(rec, dict):
                raise SystemExit(f"REFUSING: {path}:{lineno} is not a JSON object; nothing written")
            try:
                ts_str, text, prompt = extract_seth_record(rec)
            except ValueError as e:
                raise SystemExit(f"REFUSING: {path}:{lineno} {e}; nothing written")
            if not text or not text.strip():
                n_dropped_empty += 1
                continue
            text = text.strip()
            if text.startswith(TAPBACK_PREFIXES):
                n_dropped_tapback += 1
                continue
            try:
                ts = datetime.datetime.fromisoformat(ts_str)
            except (TypeError, ValueError) as e:
                raise SystemExit(f"REFUSING: {path}:{lineno} unparseable timestamp {ts_str!r} ({e}); nothing written")
            rows.append((ts, text, prompt))
    rows.sort(key=lambda r: r[0])
    return rows, n_lines, n_dropped_empty, n_dropped_tapback


def merge_seth_sources(primary_rows, extras):
    """primary_rows: list[(ts,text,prompt)] kept wholesale. extras:
    list[(label, list[(ts,text,prompt)])]. First-seen-wins by the imported
    dedup_key(ts, text) -- the SAME algorithm and per-extra stats shape
    ({"path","rows","added","duplicates"}) as
    eval_persona_evolution.py:482-499's merge_sources, extended to carry
    `prompt` through (merge_sources's own 2-tuples would drop it).
    Returns (merged sorted by time, per-extra stats)."""
    seen = {dedup_key(ts, t) for ts, t, _p in primary_rows}
    merged = list(primary_rows)
    stats = []
    for label, rows in extras:
        added = 0
        for ts, t, p in rows:
            k = dedup_key(ts, t)
            if k in seen:
                continue
            seen.add(k)
            merged.append((ts, t, p))
            added += 1
        stats.append({"path": label, "rows": len(rows), "added": added, "duplicates": len(rows) - added})
    merged.sort(key=lambda r: r[0])
    return merged, stats


def read_rejected_pool(path):
    """Read an existing preference corpus's rejected/label=False side.

    Returns (rows, n_lines, n_skipped) where `rows` is list[(prompt,
    text)] -- each row's OWN original prompt, kept as-is. Rows without a
    non-empty `rejected` string are skipped and counted (not fatal: the
    rejected side carries no Seth-authorship provenance requirement --
    AC-1.2 only constrains the chosen side)."""
    rows = []
    n_lines = 0
    n_skipped = 0
    with open(path) as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            n_lines += 1
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as e:
                raise SystemExit(f"REFUSING: {path}:{lineno} is not valid JSON ({e}); nothing written")
            if not isinstance(rec, dict):
                raise SystemExit(f"REFUSING: {path}:{lineno} is not a JSON object; nothing written")
            rejected = rec.get("rejected")
            if not isinstance(rejected, str) or not rejected.strip():
                n_skipped += 1
                continue
            rows.append((rec.get("prompt"), rejected.strip()))
    return rows, n_lines, n_skipped


def build_parser():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--primary", required=True,
                    help="best/primary Seth-authored export (kept wholesale)")
    ap.add_argument("--extra", action="append", default=[],
                    help="additional Seth-authored export; repeatable, merged in the "
                         "order given, first-seen wins on dedup collision")
    ap.add_argument("--rejected-pool", required=True, dest="rejected_pool",
                    help="existing preference corpus to read ONLY the `rejected` "
                         "field from (with each row's own prompt); no provenance "
                         "requirement on this side")
    ap.add_argument("--out-dir", required=True, dest="out_dir",
                    help="directory to write train.jsonl + manifest.json into "
                         "(must be outside the repo -- see AC-1.5)")
    ap.add_argument("--floor", type=int, default=DEFAULT_FLOOR,
                    help="refuse if the merged chosen (label=True) pool has fewer "
                         "rows than this (default: %(default)s)")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)

    for flag, path in (
        [("--primary", args.primary)]
        + [("--extra", p) for p in args.extra]
        + [("--rejected-pool", args.rejected_pool)]
    ):
        if not os.path.isfile(path):
            sys.exit(f"REFUSING: {flag} path not found or not readable: {path}; nothing written")

    primary_rows, primary_n, primary_dropped_empty, primary_dropped_tapback = read_seth_source(args.primary)

    extras = []
    total_dropped_empty = primary_dropped_empty
    total_dropped_tapback = primary_dropped_tapback
    extra_raw_reports = []
    for p in args.extra:
        rows, n, de, dtb = read_seth_source(p)
        extras.append((p, rows))
        extra_raw_reports.append({"path": p, "rows": n})
        total_dropped_empty += de
        total_dropped_tapback += dtb

    merged, extra_stats = merge_seth_sources(primary_rows, extras)

    if len(merged) < args.floor:
        sys.exit(
            f"REFUSING: merged chosen (label=True) pool has {len(merged)} rows "
            f"< --floor {args.floor}; nothing written")

    rejected_rows, rejected_n, rejected_skipped = read_rejected_pool(args.rejected_pool)
    if not rejected_rows:
        sys.exit(
            f"REFUSING: --rejected-pool {args.rejected_pool} has zero rows with a "
            f"non-empty rejected field; nothing written")

    kto_rows = []
    for ts, text, prompt in merged:
        kto_rows.append({"prompt": prompt, "completion": text, "label": True})
    for prompt, text in rejected_rows:
        kto_rows.append({"prompt": prompt, "completion": text, "label": False})

    out_dir = os.path.abspath(os.path.expanduser(args.out_dir))
    os.makedirs(out_dir, exist_ok=True)
    train_path = os.path.join(out_dir, "train.jsonl")
    with open(train_path, "w") as fh:
        for r in kto_rows:
            fh.write(json.dumps(r) + "\n")

    manifest = {
        "primary": {"path": args.primary, "rows": primary_n, "kept": len(primary_rows)},
        "extras": extra_stats,
        "dropped_empty": total_dropped_empty,
        "dropped_tapback": total_dropped_tapback,
        "rejected_pool": {
            "path": args.rejected_pool,
            "rows": rejected_n,
            "kept": len(rejected_rows),
            "skipped_no_rejected_field": rejected_skipped,
        },
        "floor": args.floor,
        "n_chosen": len(merged),
        "n_rejected": len(rejected_rows),
        "n_total": len(kto_rows),
        "out_dir": out_dir,
        "train_path": train_path,
    }
    manifest_path = os.path.join(out_dir, "manifest.json")
    with open(manifest_path, "w") as fh:
        json.dump(manifest, fh, indent=2)

    print("=" * 70)
    print(f"[merge] primary {args.primary}: {primary_n} rows read, {len(primary_rows)} kept "
          f"(after empty/tapback filtering)")
    for st in extra_stats:
        print(f"[merge] extra   {st['path']}: {st['rows']} rows read, "
              f"added {st['added']}, duplicates {st['duplicates']}")
    print(f"[merge] dropped_empty={total_dropped_empty} dropped_tapback={total_dropped_tapback}")
    print(f"[merge] rejected_pool {args.rejected_pool}: {rejected_n} rows read, "
          f"{len(rejected_rows)} kept, {rejected_skipped} skipped (no rejected field)")
    print(f"[merge] n_chosen(label=True)={len(merged)} n_rejected(label=False)={len(rejected_rows)} "
          f"n_total={len(kto_rows)}")
    print(f"[merge] wrote {train_path}")
    print(f"[merge] wrote {manifest_path}")
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
