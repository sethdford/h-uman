#!/usr/bin/env python3
"""merge_seth_preference_sources.py — merge the provenance-verified
Seth-authored preference sources named in
docs/plans/2026-09-02-persona-evolution/spec.md §3b into a training corpus,
written in TWO shapes: the paired {"prompt","chosen","rejected"} shape
(train.jsonl) that scripts/mlx_tune_train.py's own on-disk validator
(REQUIRED_PAIR_KEYS) requires, and the KTO-shaped
{"prompt","completion","label"} shape (train.kto.jsonl) the original design
(sprints/sprint-better-than-human-2026-09-05/designs/US-1.md §2) produced.

WHY BOTH SHAPES (critic re-open, 2026-09-05)
---------------------------------------------
The original design deliberately chose KTO-only because pairing a new
chosen row with a specific rejected row would assert a prompt
correspondence that doesn't exist between the two pools (see §2's "Rejected
alternative"). That reasoning is still correct about GENUINE pairing — but
scripts/mlx_tune_train.py's validate_data_dir() / _count_and_validate_jsonl()
(REQUIRED_PAIR_KEYS = ("prompt","chosen","rejected")) validates ONLY the
paired shape on disk; it converts to KTO examples itself, in-process, via
to_kto_examples(). A KTO-only train.jsonl fails that validator's key check
and can never reach the trainer. So this script now ALSO writes a paired
train.jsonl -- an explicit, deterministic, seeded ASSIGNMENT of a rejected
completion to each chosen row (build_paired_rows(), below), not a claim of
genuine prompt correspondence. Where a rejected-pool row's own original
prompt exactly matches a chosen row's rendered prompt, that (higher-fidelity)
pairing is preferred; every other chosen row is assigned a rejected
completion by a fixed-seed round-robin over the (fixed-seed-shuffled)
rejected pool, so re-running this script against the same inputs reproduces
the identical pairing. The manifest records how many rows landed in each
bucket (`pairing.matched_on_prompt` / `pairing.round_robin`) so a reader can
see the pairing's actual fidelity rather than assume it. train.kto.jsonl is
kept, unchanged in shape, as the format-agnostic ground truth of "what was
merged" independent of any pairing decision.

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

KTO shape, not paired DPO/ORPO -- why (and why we ALSO emit paired now)
------------------------------------------------------------------------
None of the Seth-authored export shapes (training_pairs, ground_truth)
carry a competing ("rejected") reply for the same context -- producing one
would require a live model call, forbidden by AC-1.6 and
scripts/check-no-resident-model.sh. KTO training does not require
prompt correspondence between labels: each chosen row keeps its own real
prompt (the training_pairs shape's preceding context messages, or the
ground_truth shape's own `incoming` string); each reused rejected row
keeps its own real, different prompt. scripts/rebalance_preference_corpus.py
already accepts and exercises this shape end-to-end (including
--match-sides) with zero changes needed here -- this remains true and
train.kto.jsonl (below) is exactly that KTO output.

But scripts/mlx_tune_train.py's on-disk validator only accepts the PAIRED
{"prompt","chosen","rejected"} shape (see the module docstring above), so a
KTO-only train.jsonl is dead on arrival at the trainer. train.jsonl is
therefore written in the PAIRED shape: each chosen row is deterministically
ASSIGNED a rejected completion (build_paired_rows()) -- preferring an exact
prompt match against --rejected-pool's own prompts where one exists, else a
fixed-seed round-robin -- not a claim that the assignment is a genuine
"a human preferred X over Y for prompt P" pair. The manifest's `pairing`
block reports how many rows fell into each bucket.

Prompt type (AC-1.2 follow-up)
--------------------------------
extract_seth_record() returns the training_pairs shape's `prompt` as the
raw list[{"role","content"}] context (its own real preceding turns, not a
synthesized string) but the ground_truth shape's `incoming` as a plain
string -- two different Python types for the same logical field. Every row
this script WRITES normalizes `prompt` to one plain string via
render_prompt(), matching how the existing corpus
(~/.human/training-data/glm-v61-pref/train.jsonl) already renders
multi-turn context: alternating "Them: "/"Seth: "-prefixed lines joined by
"\\n" (e.g. "Them: lmaooo did you even watch that game last night\\nSeth: ha
yeah I did..."), with a single-turn prompt left as the bare unprefixed text
(e.g. "I've been feeling really anxious lately") -- both forms verified by
inspecting that file's actual rows, not assumed.

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

Writes, under --out-dir:
    train.jsonl       -- PAIRED {"prompt","chosen","rejected"} shape;
                          scripts/mlx_tune_train.py's on-disk validator reads
                          THIS file.
    train.kto.jsonl   -- KTO {"prompt","completion","label"} shape; the
                          original per-source merge output, unchanged.
    manifest.json     -- counts, per-source provenance, pairing method +
                          counts; never message text (AC-1.5).
"""
import argparse
import datetime
import json
import re
import hashlib
import os
import random
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

    Provenance is checked via each turn's `role` field (last.get("role") ==
    "assistant"), NOT the original chat.db `is_from_me` flag, because
    scripts/extract_imessage_pairs.py's extract_training_pairs() translates
    is_from_me -> role at export time and does not carry is_from_me itself
    into the exported JSON (see its `role = "assistant" if ctx["is_from_me"]
    else "user"` mapping) -- role is the only Seth-authorship signal these
    export shapes still carry by the time this script reads them.
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
    AC-1.2 only constrains the chosen side). The rejected side is trusted
    by CONVENTION, not verified: per AC-1.2 / designs/US-1.md §4, a
    `rejected` field is assumed to be model output (never Seth-authored)
    because the corpora this script is pointed at (e.g.
    ~/.human/training-data/glm-v61-pref/train.jsonl) are themselves built
    with that invariant (scripts/build_v6_preference_corpus.py's own
    provenance work) -- this function does not and cannot independently
    re-verify that invariant for an arbitrary --rejected-pool path."""
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


# ---------------------------------------------------------------------------
# Prompt rendering -- ONE plain-string shape for every row this script
# writes, regardless of which source shape it came from (see the module
# docstring's "Prompt type (AC-1.2 follow-up)" section).
# ---------------------------------------------------------------------------


def split_valid_rows(rows, frac):
    """Deterministic held-out split of PAIRED rows: a row goes to valid.jsonl iff
    sha256(prompt + chosen) % round(1/frac) == 0. Content-keyed (not positional)
    so a regenerated corpus keeps the same validation rows and val loss stays
    comparable across regenerations. Never empties the training side."""
    if not frac or frac <= 0:
        return [], list(rows)
    mod = max(1, int(round(1.0 / frac)))
    val, train = [], []
    for r in rows:
        key = (str(r.get("prompt", "")) + "\x1f" + str(r.get("chosen", ""))).encode("utf-8")
        h = int.from_bytes(hashlib.sha256(key).digest()[:8], "big")
        (val if h % mod == 0 else train).append(r)
    if not train:
        return [], list(rows)
    return val, train


def render_config(template_path, out_dir):
    """The template's `data:` line rewritten to out_dir; everything else verbatim
    (same rewrite train-glm-adapter.sh applies for its rebalanced copy)."""
    with open(template_path) as fh:
        text = fh.read()
    if not re.search(r"^data:.*$", text, flags=re.M):
        raise SystemExit(f"REFUSING: --config-template {template_path} has no 'data:' line; nothing written")
    return re.sub(r"^data:.*$", f"data: {out_dir}", text, count=1, flags=re.M)


def render_prompt(prompt):
    """Normalize a prompt value from either accepted export shape (or the
    --rejected-pool's own `prompt` field) into ONE plain string.

    Accepts:
      - a plain string (ground_truth shape's `incoming`, or an already
        string-shaped --rejected-pool prompt) -- returned stripped, as-is;
      - list[{"role": "user"|"assistant", "content": str}] (training_pairs
        shape's context turns, extract_seth_record's `msgs[:-1]`) --
        rendered to match how ~/.human/training-data/glm-v61-pref/train.jsonl
        already renders multi-turn context: a single turn is left as the
        bare unprefixed text (that file's single-incoming-message rows,
        e.g. "I've been feeling really anxious lately"); two or more turns
        are joined by "\\n" as alternating "Them: "/"Seth: "-prefixed lines
        (that file's multi-turn rows, e.g. "Them: lmaooo did you even watch
        that game last night\\nSeth: ha yeah I did..."), role "assistant"
        (Seth's own prior turns) -> "Seth: ", anything else -> "Them: ";
      - None or any other value -- coerced to "" (never emits a non-string
        `prompt`; a missing --rejected-pool prompt must not become a JSON
        `null` that fails an isinstance(str) check downstream).
    """
    if prompt is None:
        return ""
    if isinstance(prompt, str):
        return prompt.strip()
    if isinstance(prompt, list):
        if len(prompt) == 0:
            return ""
        if len(prompt) == 1:
            turn = prompt[0]
            content = turn.get("content") if isinstance(turn, dict) else turn
            return (content or "").strip()
        lines = []
        for turn in prompt:
            if isinstance(turn, dict):
                speaker = "Seth" if turn.get("role") == "assistant" else "Them"
                content = (turn.get("content") or "").strip()
            else:
                speaker = "Them"
                content = str(turn).strip()
            lines.append(f"{speaker}: {content}")
        return "\n".join(lines)
    return str(prompt).strip()


# ---------------------------------------------------------------------------
# Paired-shape assignment -- scripts/mlx_tune_train.py's on-disk validator
# (REQUIRED_PAIR_KEYS) requires {"prompt","chosen","rejected"} rows; see the
# module docstring's "WHY BOTH SHAPES" section for why this is a
# deterministic ASSIGNMENT, not a claim of genuine paired preference.
# ---------------------------------------------------------------------------

PAIRING_SEED = 20260905  # fixed -- re-running against the same inputs must
                          # reproduce the identical pairing (US-1 critic fix)


def build_paired_rows(chosen_rows, rejected_rows, seed=PAIRING_SEED):
    """chosen_rows: list[(ts, prompt_str, text)], already time-sorted and
    prompt-rendered (render_prompt already applied). rejected_rows:
    list[(prompt_str, text)], the --rejected-pool's own rows, prompt already
    rendered.

    For each chosen row, prefer a rejected completion whose OWN original
    prompt exactly equals the chosen row's rendered prompt (round-robin
    within that prompt's own candidates, so repeats rotate rather than
    always picking the first); otherwise assign the next completion from a
    fixed-seed shuffle of the FULL rejected pool, round-robin. Both cursors
    are deterministic given `seed` and the input order, so re-running this
    script against unchanged inputs reproduces the identical output file
    byte-for-byte.

    Returns (paired_rows, stats) where paired_rows is
    list[{"prompt","chosen","rejected"}] in chosen_rows' order and stats is
    {"matched_on_prompt": int, "round_robin": int}.
    """
    by_prompt = {}
    for p, t in rejected_rows:
        by_prompt.setdefault(p, []).append(t)

    rng = random.Random(seed)
    shuffled_pool = [t for _, t in rejected_rows]
    rng.shuffle(shuffled_pool)

    prompt_cursor = {p: 0 for p in by_prompt}
    round_robin_cursor = 0
    n_matched = 0
    n_round_robin = 0
    paired = []
    for _ts, prompt, chosen_text in chosen_rows:
        candidates = by_prompt.get(prompt)
        if candidates:
            idx = prompt_cursor[prompt] % len(candidates)
            rejected_text = candidates[idx]
            prompt_cursor[prompt] += 1
            n_matched += 1
        else:
            rejected_text = shuffled_pool[round_robin_cursor % len(shuffled_pool)]
            round_robin_cursor += 1
            n_round_robin += 1
        paired.append({"prompt": prompt, "chosen": chosen_text, "rejected": rejected_text})
    return paired, {"matched_on_prompt": n_matched, "round_robin": n_round_robin}


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
    ap.add_argument("--valid-frac", type=float, default=0.0, dest="valid_frac",
                    help="fraction of PAIRED rows held out into valid.jsonl, chosen "
                         "deterministically by content hash so regenerating the corpus "
                         "keeps the same held-out rows (default 0 = all rows in train.jsonl, "
                         "the US-1 contract; pass e.g. 0.05 for a trainable dir). The nightly "
                         "candidate stage (train-glm-adapter.sh) requires valid.jsonl.")
    ap.add_argument("--config-template", default=None, dest="config_template",
                    help="mlx_lm-shaped YAML whose 'data:' line is rewritten to --out-dir "
                         "and written as <out-dir>/config.yaml, so the directory is "
                         "trainable as-is (HU_RETRAIN_MLXTUNE_DATA_DIR/_CONFIG)")
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

    # Render every prompt to ONE plain-string shape (AC-1.2 follow-up) before
    # it lands in either output file -- see render_prompt()'s docstring.
    chosen_for_output = [(ts, render_prompt(prompt), text) for ts, text, prompt in merged]
    rejected_for_output = [(render_prompt(prompt), text) for prompt, text in rejected_rows]

    kto_rows = []
    for ts, prompt_str, text in chosen_for_output:
        kto_rows.append({"prompt": prompt_str, "completion": text, "label": True})
    for prompt_str, text in rejected_for_output:
        kto_rows.append({"prompt": prompt_str, "completion": text, "label": False})

    paired_rows, pairing_stats = build_paired_rows(chosen_for_output, rejected_for_output)
    n_paired_total = len(paired_rows)
    valid_rows, paired_rows = split_valid_rows(paired_rows, args.valid_frac)

    out_dir = os.path.abspath(os.path.expanduser(args.out_dir))
    os.makedirs(out_dir, exist_ok=True)

    valid_path = None
    if valid_rows:
        valid_path = os.path.join(out_dir, "valid.jsonl")
        with open(valid_path, "w") as fh:
            for r in valid_rows:
                fh.write(json.dumps(r) + "\n")

    config_path = None
    if args.config_template:
        config_path = os.path.join(out_dir, "config.yaml")
        with open(config_path, "w") as fh:
            fh.write(render_config(args.config_template, out_dir))

    # train.jsonl: PAIRED shape -- what scripts/mlx_tune_train.py's on-disk
    # validator (REQUIRED_PAIR_KEYS) actually requires. See the module
    # docstring's "WHY BOTH SHAPES" section.
    train_path = os.path.join(out_dir, "train.jsonl")
    with open(train_path, "w") as fh:
        for r in paired_rows:
            fh.write(json.dumps(r) + "\n")

    # train.kto.jsonl: the original KTO-shaped output, unchanged in shape --
    # the format-agnostic record of what was actually merged.
    kto_path = os.path.join(out_dir, "train.kto.jsonl")
    with open(kto_path, "w") as fh:
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
        "valid_frac": args.valid_frac,
        "n_valid": len(valid_rows),
        "valid_path": valid_path,
        "config_template": args.config_template,
        "config_path": config_path,
        "n_chosen": len(merged),
        "n_rejected": len(rejected_rows),
        "n_total": len(kto_rows),
        "pairing": {
            "method": "match-on-rendered-prompt-else-fixed-seed-round-robin",
            "seed": PAIRING_SEED,
            "matched_on_prompt": pairing_stats["matched_on_prompt"],
            "round_robin": pairing_stats["round_robin"],
            "n_paired_rows": n_paired_total, "n_train_rows": len(paired_rows),
        },
        "out_dir": out_dir,
        "train_path": train_path,
        "kto_path": kto_path,
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
    print(f"[merge] pairing: matched_on_prompt={pairing_stats['matched_on_prompt']} "
          f"round_robin={pairing_stats['round_robin']} (seed={PAIRING_SEED})")
    print(f"[merge] wrote {train_path} (paired shape)")
    print(f"[merge] wrote {kto_path} (KTO shape)")
    print(f"[merge] wrote {manifest_path}")
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
