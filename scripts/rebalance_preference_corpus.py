#!/usr/bin/env python3
"""rebalance_preference_corpus.py — pull a preference corpus's CHOSEN side
back toward Seth's measured casing/punctuation distribution.

WHY THIS EXISTS (2026-09-04): the v6 ORPO preference corpus taught the model
an 86% lowercase-start habit that has nothing to do with the four targeted
failure modes (performed wit, generic platitude, flat-where-warm,
over-elaboration — see scripts/build_v6_preference_corpus.py). Read-only
audit of that corpus found the CHOSEN side is 77.5% lowercase-start / 25.1%
terminal-punct / median 50 chars, against a REJECTED side of 6.5% / 58.9% /
137 chars and a real-Seth measurement of ~6-9% lowercase-start / ~18-25%
terminal-punct. ORPO learned "start lowercase" as a 71-point side effect of
learning "be terser than the assistant register" — the corpus confounds
brevity with casing because its sources (cycle-4 curated replies,
generated_v2, arena self-play) are lowercase by construction, not because
Seth types that way.

WHAT THIS SCRIPT DOES AND DOES NOT DO
--------------------------------------
Rebalances the CHOSEN side of a preference file toward a target
lowercase-start rate and terminal-punctuation rate by SAMPLING which rows
keep their original casing/punctuation and which get a deterministic
transform applied (sentence-case the first letter, or add/strip a trailing
".", "!", "?", "..."). It NEVER invents, paraphrases, or otherwise changes
the words of a reply — only the casing of the first letter and the presence
of a terminal punctuation mark. The REJECTED side (or the KTO label=False
side) is never touched; it is measured only, as the reference the CHOSEN
side is being pulled toward matching (not exceeding).

Refuses (exit non-zero, writes nothing) when:
  - neither --target-lowercase/--target-punct nor a readable style card is
    available (see .claude/rules/no-number-without-a-measurement.md);
  - the input has zero rebalanceable ("chosen"/KTO-True) rows;
  - after rebalancing, the chosen-vs-rejected margin on lowercase-start
    still exceeds --max-lowercase-margin (default 0.15) — a persistently
    wide gap means sampling could not close it (e.g. every candidate row is
    already the same casing) and shipping the corpus anyway would silently
    under-deliver the fix this script exists to make.

Schemas accepted (auto-detected per row):
  - preference/ORPO/DPO: {"prompt": "...", "chosen": "...", "rejected": "..."}
    Chosen side is "chosen"; rejected side is "rejected" (same row).
  - KTO: {"prompt": "...", "completion": "...", "label": true|false}
    Chosen side is every row where label is truthy; rejected side is every
    row where label is falsy (different rows — KTO labels are unpaired).

Casing/punctuation definitions are the SAME functions the persona's style
card uses (scripts/eval_persona_evolution.py's starts_lowercase /
terminal_punctuation) — see .claude/rules/ (style_card_single_source memory
note): a second hand-rolled definition of "starts lowercase" is exactly the
kind of drift that caused the 2026-07 deliberation-leak diagnosis.

Usage:
    scripts/rebalance_preference_corpus.py \\
        --input ~/.human/training-data/glm-v6-pref/train.jsonl \\
        --output /tmp/train.rebalanced.jsonl

    scripts/rebalance_preference_corpus.py --input <file> --dry-run
        # measure + report only; writes nothing regardless of the outcome
"""
import argparse
import json
import os
import random
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_persona_evolution import (  # noqa: E402
    starts_lowercase as _starts_lowercase_or_none,
)
from eval_persona_evolution import terminal_punctuation  # noqa: E402

DEFAULT_STYLE_CARD = os.path.expanduser("~/.human/personas/seth.style-card.json")
DEFAULT_MAX_LOWERCASE_MARGIN = 0.15
TERMINAL_MARKS = ".!?"


# ---------------------------------------------------------------------------
# axis definitions -- reuse the style-card's own functions, never re-derive
# ---------------------------------------------------------------------------


def is_lowercase_start(text):
    """True/False/None (no alphabetic char at all -- excluded from the rate,
    same convention as the style card)."""
    return _starts_lowercase_or_none(text or "")


def has_terminal_punct(text):
    return terminal_punctuation(text or "") != "none"


def lowercase_rate(texts):
    vals = [v for v in (is_lowercase_start(t) for t in texts) if v is not None]
    if not vals:
        return 0.0, 0
    return sum(1 for v in vals if v) / len(vals), len(vals)


def punct_rate(texts):
    if not texts:
        return 0.0, 0
    vals = [has_terminal_punct(t) for t in texts]
    return sum(1 for v in vals if v) / len(vals), len(vals)


def median_length(texts):
    if not texts:
        return 0
    return statistics.median(len(t) for t in texts)


def axis_report(texts):
    lc, lc_n = lowercase_rate(texts)
    pt, pt_n = punct_rate(texts)
    return {
        "n": len(texts),
        "lowercase_start_rate": round(lc, 4),
        "lowercase_start_n": lc_n,
        "terminal_punct_rate": round(pt, 4),
        "median_length": median_length(texts),
    }


# ---------------------------------------------------------------------------
# deterministic text transforms -- casing/punctuation ONLY, never wording
# ---------------------------------------------------------------------------


def to_sentence_case(text):
    """Uppercase the first alphabetic character; no-op if there is none."""
    for i, ch in enumerate(text):
        if ch.isalpha():
            if ch == ch.upper():
                return text
            return text[:i] + ch.upper() + text[i + 1:]
        if ch.isdigit():
            return text
    return text


def to_lowercase_start(text):
    """Lowercase the first alphabetic character; no-op if there is none."""
    for i, ch in enumerate(text):
        if ch.isalpha():
            if ch == ch.lower():
                return text
            return text[:i] + ch.lower() + text[i + 1:]
        if ch.isdigit():
            return text
    return text


def add_terminal_punct(text, mark="."):
    """Append a terminal mark if the text has none. Strips trailing
    whitespace before appending (does not otherwise touch the text)."""
    stripped = text.rstrip()
    if not stripped or terminal_punctuation(stripped) != "none":
        return text
    return stripped + mark


def strip_terminal_punct(text):
    """Remove trailing terminal punctuation (., !, ?, ..., the unicode
    ellipsis) and any whitespace it leaves behind. No-op if there is none."""
    stripped = text.rstrip()
    if not stripped:
        return text
    while stripped and terminal_punctuation(stripped) != "none":
        if stripped.endswith("…"):
            stripped = stripped[:-1].rstrip()
            continue
        if stripped.endswith("..."):
            stripped = stripped[:-3].rstrip()
            continue
        stripped = stripped[:-1].rstrip()
    return stripped


# ---------------------------------------------------------------------------
# the sampler -- decide WHICH rows change, deterministically, via a seeded RNG
# ---------------------------------------------------------------------------


def resample_axis(texts, flag_fn, target_rate, transform_to_false, transform_to_true, rng):
    """Mutate `texts` (a list, in place via return value) so its flag_fn rate
    moves toward target_rate, by transforming the minimum number of rows
    needed -- chosen via `rng.sample` so the same seed always picks the same
    rows. Rows where flag_fn returns None (no signal) are left untouched and
    excluded from both the population and the rate.

    Returns (new_texts, {"before": rate, "after": rate, "changed": n}).
    """
    eligible = [i for i, t in enumerate(texts) if flag_fn(t) is not None]
    n = len(eligible)
    out = list(texts)
    if n == 0:
        return out, {"before": 0.0, "after": 0.0, "changed": 0, "n": 0}

    idx_true = [i for i in eligible if flag_fn(texts[i])]
    idx_false = [i for i in eligible if not flag_fn(texts[i])]
    before_rate = len(idx_true) / n
    target_count = round(target_rate * n)
    changed = 0

    if len(idx_true) > target_count:
        need = len(idx_true) - target_count
        pick = rng.sample(idx_true, min(need, len(idx_true)))
        for i in pick:
            out[i] = transform_to_false(out[i])
            changed += 1
    elif len(idx_true) < target_count:
        need = target_count - len(idx_true)
        pick = rng.sample(idx_false, min(need, len(idx_false)))
        for i in pick:
            out[i] = transform_to_true(out[i])
            changed += 1

    after_true = sum(1 for i in eligible if flag_fn(out[i]))
    after_rate = after_true / n
    return out, {"before": round(before_rate, 4), "after": round(after_rate, 4),
                 "changed": changed, "n": n}


def rebalance_chosen_texts(texts, target_lowercase, target_punct, seed):
    """Apply both axis transforms to a list of chosen-side texts. Returns
    (new_texts, {"lowercase_start": {...}, "terminal_punct": {...}})."""
    rng = random.Random(seed)
    texts, lc_report = resample_axis(
        texts, is_lowercase_start, target_lowercase,
        to_sentence_case, to_lowercase_start, rng)
    texts, pt_report = resample_axis(
        texts, lambda t: has_terminal_punct(t), target_punct,
        strip_terminal_punct, add_terminal_punct, rng)
    return texts, {"lowercase_start": lc_report, "terminal_punct": pt_report}


# ---------------------------------------------------------------------------
# schema detection + row indexing
# ---------------------------------------------------------------------------


def norm(s):
    return (s or "").strip()


def is_kto_label_true(label):
    if isinstance(label, bool):
        return label
    if isinstance(label, (int, float)):
        return bool(label)
    if isinstance(label, str):
        return label.strip().lower() in ("true", "chosen", "1", "yes", "desirable")
    return False


def load_jsonl(path):
    rows = []
    with open(path) as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as e:
                raise SystemExit(f"REFUSING: {path}:{lineno} is not valid JSON ({e}); nothing written")
    return rows


def index_rows(rows):
    """Returns (chosen_index, rejected_texts) where chosen_index is a list
    of (row_i, field_name) pairs identifying every rebalanceable text, and
    rejected_texts is the flat list of reference (never-modified) texts."""
    chosen_index = []
    rejected_texts = []
    for i, r in enumerate(rows):
        if not isinstance(r, dict):
            raise SystemExit(f"REFUSING: row {i} is not a JSON object; nothing written")
        if "chosen" in r and "rejected" in r:
            chosen_index.append((i, "chosen"))
            rejected_texts.append(norm(r.get("rejected")))
        elif "completion" in r and "label" in r:
            if is_kto_label_true(r.get("label")):
                chosen_index.append((i, "completion"))
            else:
                rejected_texts.append(norm(r.get("completion")))
        else:
            raise SystemExit(
                f"REFUSING: row {i} matches neither the preference "
                f"{{chosen,rejected}} shape nor the KTO {{completion,label}} "
                f"shape; nothing written")
    return chosen_index, rejected_texts


def get_indexed_texts(rows, index):
    return [norm(rows[i].get(field)) for i, field in index]


def set_indexed_texts(rows, index, texts):
    for (i, field), t in zip(index, texts):
        rows[i][field] = t


# ---------------------------------------------------------------------------
# style-card target resolution
# ---------------------------------------------------------------------------


def load_style_card(path):
    if not path or not os.path.isfile(path):
        return None
    try:
        return json.load(open(path))
    except (json.JSONDecodeError, OSError):
        return None


def resolve_targets(target_lowercase, target_punct, style_card_path):
    """Returns (target_lowercase, target_punct, provenance_dict). Raises
    SystemExit (REFUSING, nothing written) if a target is missing and the
    style card cannot supply it."""
    provenance = {}
    card = None
    if target_lowercase is None or target_punct is None:
        card = load_style_card(style_card_path)

    if target_lowercase is not None:
        provenance["lowercase_start"] = f"--target-lowercase={target_lowercase}"
    else:
        if card is None:
            raise SystemExit(
                f"REFUSING: no --target-lowercase given and no readable style "
                f"card at {style_card_path}; nothing written")
        try:
            target_lowercase = float(card["axes"]["lowercase_start_rate"]["value"])
        except (KeyError, TypeError, ValueError) as e:
            raise SystemExit(
                f"REFUSING: style card {style_card_path} has no usable "
                f"axes.lowercase_start_rate.value ({e}); nothing written")
        provenance["lowercase_start"] = (
            f"style_card:axes.lowercase_start_rate.value={target_lowercase:.4f} "
            f"({style_card_path})")

    if target_punct is not None:
        provenance["terminal_punct"] = f"--target-punct={target_punct}"
    else:
        if card is None:
            raise SystemExit(
                f"REFUSING: no --target-punct given and no readable style "
                f"card at {style_card_path}; nothing written")
        try:
            no_term = float(card["axes"]["no_terminal_punct_rate"]["value"])
        except (KeyError, TypeError, ValueError) as e:
            raise SystemExit(
                f"REFUSING: style card {style_card_path} has no usable "
                f"axes.no_terminal_punct_rate.value ({e}); nothing written")
        target_punct = round(1.0 - no_term, 6)
        provenance["terminal_punct"] = (
            f"style_card:1-axes.no_terminal_punct_rate.value={target_punct:.4f} "
            f"({style_card_path})")

    return target_lowercase, target_punct, provenance


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------


def print_report(before_chosen, after_chosen, rejected, targets, provenance):
    def line(label, before, after, ref):
        margin_before = round(abs(before - ref), 4)
        margin_after = round(abs(after - ref), 4)
        print(f"  {label:22} chosen_before={before:<8.4f} chosen_after={after:<8.4f} "
              f"rejected={ref:<8.4f} margin_before={margin_before:<8.4f} margin_after={margin_after:<8.4f}")
        return margin_before, margin_after

    print("[rebalance] targets:")
    for k, v in provenance.items():
        print(f"  {k}: {v}")

    b_lc, _ = lowercase_rate(before_chosen)
    a_lc, _ = lowercase_rate(after_chosen)
    r_lc, _ = lowercase_rate(rejected)
    b_pt, _ = punct_rate(before_chosen)
    a_pt, _ = punct_rate(after_chosen)
    r_pt, _ = punct_rate(rejected)
    b_len = median_length(before_chosen)
    a_len = median_length(after_chosen)
    r_len = median_length(rejected)

    print(f"[rebalance] n_chosen={len(before_chosen)} n_rejected={len(rejected)}")
    print("[rebalance] axis                  chosen_before chosen_after rejected  margin_before margin_after")
    lc_margins = line("lowercase_start_rate", b_lc, a_lc, r_lc)
    pt_margins = line("terminal_punct_rate", b_pt, a_pt, r_pt)
    print(f"  {'median_length':22} chosen_before={b_len:<8} chosen_after={a_len:<8} rejected={r_len:<8}")
    return {
        "lowercase_start_rate": {"chosen_before": round(b_lc, 4), "chosen_after": round(a_lc, 4),
                                  "rejected": round(r_lc, 4),
                                  "margin_before": lc_margins[0], "margin_after": lc_margins[1]},
        "terminal_punct_rate": {"chosen_before": round(b_pt, 4), "chosen_after": round(a_pt, 4),
                                 "rejected": round(r_pt, 4),
                                 "margin_before": pt_margins[0], "margin_after": pt_margins[1]},
        "median_length": {"chosen_before": b_len, "chosen_after": a_len, "rejected": r_len},
    }


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def build_parser():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", required=True, help="preference or KTO JSONL file to read")
    ap.add_argument("--output", default=None,
                    help="rebalanced JSONL to write (required unless --dry-run)")
    ap.add_argument("--sidecar", default=None,
                    help="before/after stats JSON (default: <output>.stats.json)")
    ap.add_argument("--target-lowercase", type=float, default=None,
                    help="target lowercase-start rate for the chosen side "
                         "(default: read from --style-card, e.g. ~0.08)")
    ap.add_argument("--target-punct", type=float, default=None,
                    help="target terminal-punctuation rate for the chosen side "
                         "(default: read from --style-card, e.g. ~0.22)")
    ap.add_argument("--style-card", default=DEFAULT_STYLE_CARD,
                    help="style-card/v2 JSON to read targets from when not given on the CLI")
    ap.add_argument("--max-lowercase-margin", type=float, default=DEFAULT_MAX_LOWERCASE_MARGIN,
                    help="refuse if |chosen_after - rejected| on lowercase-start "
                         "exceeds this (default: %(default)s)")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--dry-run", action="store_true",
                    help="measure and report only; write nothing")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)

    if not args.dry_run and not args.output:
        sys.exit("FATAL: --output is required unless --dry-run")

    if not os.path.isfile(args.input):
        sys.exit(f"REFUSING: input not found: {args.input}; nothing written")

    rows = load_jsonl(args.input)
    if not rows:
        sys.exit(f"REFUSING: {args.input} has zero rows; nothing written")

    chosen_index, rejected_texts = index_rows(rows)
    if not chosen_index:
        sys.exit(f"REFUSING: {args.input} has zero chosen/KTO-true rows to rebalance; nothing written")

    target_lowercase, target_punct, provenance = resolve_targets(
        args.target_lowercase, args.target_punct, args.style_card)

    before_chosen = get_indexed_texts(rows, chosen_index)
    after_chosen, axis_reports = rebalance_chosen_texts(
        before_chosen, target_lowercase, target_punct, args.seed)

    print("=" * 70)
    print(f"[rebalance] input={args.input}")
    stats = print_report(before_chosen, after_chosen, rejected_texts, None, provenance)
    print("=" * 70)

    lc_margin_after = stats["lowercase_start_rate"]["margin_after"]
    if lc_margin_after > args.max_lowercase_margin:
        sys.exit(
            f"REFUSING: after-rebalance lowercase-start margin {lc_margin_after:.4f} "
            f"still exceeds --max-lowercase-margin {args.max_lowercase_margin}; "
            f"sampling could not close the gap (are chosen/rejected already "
            f"nearly identical on this axis?); nothing written")

    if args.dry_run:
        print("[rebalance] --dry-run: nothing written")
        return 0

    set_indexed_texts(rows, chosen_index, after_chosen)

    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.output, "w") as fh:
        for r in rows:
            fh.write(json.dumps(r) + "\n")

    sidecar_path = args.sidecar or (args.output + ".stats.json")
    sidecar = {
        "input": args.input,
        "output": args.output,
        "seed": args.seed,
        "targets": {"lowercase_start_rate": target_lowercase, "terminal_punct_rate": target_punct},
        "target_provenance": provenance,
        "max_lowercase_margin": args.max_lowercase_margin,
        "n_rows": len(rows),
        "n_chosen": len(chosen_index),
        "n_rejected": len(rejected_texts),
        "axis_resampling": axis_reports,
        "stats": stats,
    }
    with open(sidecar_path, "w") as fh:
        json.dump(sidecar, fh, indent=2)

    print(f"[rebalance] wrote {args.output}")
    print(f"[rebalance] wrote {sidecar_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
