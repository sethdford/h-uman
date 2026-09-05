#!/usr/bin/env python3
"""authorship_promotion_gate.py — per-cycle LUAR promotion gate (US-2).

A pure predicate, plus two loaders that turn an on-disk measurement into
this predicate's inputs. Mirrors casing_probe.py's shape in this same
directory: a pure function that returns a verdict dict, plus a `_from_file`
/ loader that raises SystemExit on missing evidence, plus a thin CLI.

WHY THIS EXISTS (2026-09): the v6 authorship regression (twin 0.70 -> 0.625,
casing-confounded) reached a staged candidate with no automated check that
it had gotten WORSE than what was already serving. This module is that
check. It never trains, never generates, never loads a model — it is pure
arithmetic on numbers `authorship_gap.py` (via score_candidate_offline.py)
already measured.

Two enforcement points consume this module:
  - scripts/blind_ab/score_candidate_offline.py — adds a `promotion_gate`
    field to its own nightly output (informational at that call site; that
    script never promotes anything).
  - scripts/m3_promote.py:cmd_promote() — the ONE place that performs a live
    LoRA swap + adapter_registry.record_promotion(). This is where the gate
    actually BLOCKS.
  - scripts/register_v6_adapter.py annotates (never blocks) the same
    verdict onto its own registry row, for human visibility before anyone
    runs m3_promote.py at all.

Contract (.claude/rules/no-number-without-a-measurement.md): a missing or
malformed input REFUSES loudly (SystemExit, nothing computed) rather than
silently defaulting to a number that could produce a false PASS.
`decide_promotion()` itself has exactly two possible verdicts, PASS and
BLOCK — INCONCLUSIVE is a property of the *loader* raising before
`decide_promotion` is ever called, never a return value of the predicate.

No message text, phone numbers, or names ever pass through this module —
every input is a float, a path, or a fixed string, mirroring
authorship_gap.py's own output shape.
"""
import argparse
import glob
import json
import os
import sys

DEFAULT_LOGS_GLOB = os.path.expanduser("~/.human/logs/candidate-authorship-*.json")


def _as_finite_float(v):
    """Convert to float, rejecting None, non-numeric strings, NaN, and
    +/-inf. A None/NaN value that survives as if it were a real number is
    exactly the class of bug .claude/rules/no-number-without-a-measurement.md
    catalogs — this is the single choke point that closes it for this
    module's two loaders."""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    if f != f or f in (float("inf"), float("-inf")):  # NaN / +-inf
        return None
    return f


def decide_promotion(candidate_twin, serving_twin, floor, min_gain=0.0):
    """Pure. All three args are floats already read from authorship_gap.py's
    own JSON (never hardcoded 0.625/0.70/0.62 — those numbers are context,
    not constants). Returns {"verdict": "PASS"|"BLOCK", "reason": str,
    "candidate_twin", "serving_twin", "floor", "delta"}.

    Never returns INCONCLUSIVE — that state is a property of MISSING
    inputs, decided by the caller (load_gate_inputs_from_score_json /
    load_gate_inputs_from_gap_jsons) before this function is ever called.

    AC-2.2's literal boundary: BLOCK when new-cycle twin <= previous-cycle
    twin (a plain `<=`, not `<` — an off-by-one here would be invisible in
    every other test, so the boundary is pinned by its own test), OR
    new-cycle twin < the measured floor. `min_gain` defaults to 0.0 (the
    literal AC-2.2 boundary) and is a keyword, not a magic number buried in
    the body, so a later story can require a minimum step size without
    touching this function's control flow.
    """
    delta = round(candidate_twin - serving_twin, 4)
    base = {
        "candidate_twin": candidate_twin,
        "serving_twin": serving_twin,
        "floor": floor,
        "delta": delta,
    }
    if candidate_twin < floor:
        return {**base, "verdict": "BLOCK", "reason": "below_floor"}
    if candidate_twin <= serving_twin + min_gain:
        return {**base, "verdict": "BLOCK", "reason": "regression_vs_prior"}
    return {**base, "verdict": "PASS", "reason": "twin_improved_over_prior_above_floor"}


def load_gate_inputs_from_score_json(path):
    """Primary path (AC-2.1): reads a score_candidate_offline.py comparison
    JSON directly — candidate_twin/serving_twin/floor all come from ONE
    file, measured the same night with the same seed/splits/other-senders
    draw, which removes the day-to-day floor-redraw confound a
    two-separate-files comparison would have.

    Raises SystemExit ("INCONCLUSIVE: ...") if the file is missing, fails
    to parse, or lacks comparison.twin_candidate / comparison.twin_serving /
    candidate.floor_seth_vs_other_humans.mean as finite floats. This is the
    generalized, VERIFIED form of AC-2.3's "authorship_gap.py refused" /
    "either JSON is missing/lacks a measurement" — the literal `n` field
    inside authorship_gap.py's own stat blocks is always 200 (the bootstrap
    resample count), not a measured-sample-size signal, so checking it
    would silently pass every case this loader is meant to catch. See
    designs/US-2.md §0 for the full verification.
    """
    if not path or not os.path.isfile(path):
        raise SystemExit(f"INCONCLUSIVE: score JSON not found: {path}; nothing to gate on")
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        raise SystemExit(f"INCONCLUSIVE: could not parse {path} ({type(e).__name__}: {e})")

    comparison = data.get("comparison") if isinstance(data, dict) else None
    candidate = data.get("candidate") if isinstance(data, dict) else None
    if not isinstance(comparison, dict) or not isinstance(candidate, dict):
        raise SystemExit(
            f"INCONCLUSIVE: {path} missing comparison/candidate blocks; nothing to gate on")

    cand_twin = _as_finite_float(comparison.get("twin_candidate"))
    serv_twin = _as_finite_float(comparison.get("twin_serving"))
    floor_block = candidate.get("floor_seth_vs_other_humans")
    floor = _as_finite_float(floor_block.get("mean")) if isinstance(floor_block, dict) else None

    if cand_twin is None or serv_twin is None or floor is None:
        raise SystemExit(
            f"INCONCLUSIVE: {path} missing/non-finite twin_candidate, twin_serving, "
            "or candidate.floor_seth_vs_other_humans.mean; nothing to gate on")

    return {
        "candidate_twin": cand_twin,
        "serving_twin": serv_twin,
        "floor": floor,
        "candidate_adapter": data.get("candidate_adapter"),
        "serving_adapter": data.get("serving_adapter"),
        "candidate_gap": candidate,
        "serving_gap": data.get("serving"),
        "score_json_path": str(path),
    }


def load_gate_inputs_from_gap_jsons(candidate_path, serving_path):
    """Secondary path: two separate authorship_gap.py --out files, e.g.
    authorship_nightly.sh's daily ~/.human/logs/authorship-gap-<date>.json
    for 'serving' when no candidate trained tonight. Same
    missing/malformed -> SystemExit("INCONCLUSIVE: ...") contract as the
    primary loader. The floor is read from the CANDIDATE side (the same
    run's own floor draw), matching how score_candidate_offline.py's own
    `candidate` block carries the floor used for that comparison.
    """
    def _load_gap(path, label):
        if not path or not os.path.isfile(path):
            raise SystemExit(
                f"INCONCLUSIVE: {label} gap JSON not found: {path}; nothing to gate on")
        try:
            with open(path) as f:
                return json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            raise SystemExit(
                f"INCONCLUSIVE: could not parse {label} gap JSON {path} "
                f"({type(e).__name__}: {e})")

    cand = _load_gap(candidate_path, "candidate")
    serv = _load_gap(serving_path, "serving")

    cand_twin_block = cand.get("twin_seth_vs_adapter") if isinstance(cand, dict) else None
    serv_twin_block = serv.get("twin_seth_vs_adapter") if isinstance(serv, dict) else None
    floor_block = cand.get("floor_seth_vs_other_humans") if isinstance(cand, dict) else None

    cand_twin = _as_finite_float(cand_twin_block.get("mean")) if isinstance(cand_twin_block, dict) else None
    serv_twin = _as_finite_float(serv_twin_block.get("mean")) if isinstance(serv_twin_block, dict) else None
    floor = _as_finite_float(floor_block.get("mean")) if isinstance(floor_block, dict) else None

    if cand_twin is None or serv_twin is None or floor is None:
        raise SystemExit(
            "INCONCLUSIVE: candidate/serving gap JSON missing/non-finite "
            "twin_seth_vs_adapter.mean or floor_seth_vs_other_humans.mean; nothing to gate on")

    return {
        "candidate_twin": cand_twin,
        "serving_twin": serv_twin,
        "floor": floor,
        "candidate_gap": cand,
        "serving_gap": serv,
        "candidate_json_path": str(candidate_path),
        "serving_json_path": str(serving_path),
    }


def _find_latest_score_json(adapter_path, pattern=None):
    """Return the newest candidate-authorship-*.json whose own
    `candidate_adapter` field equals `adapter_path`, or None if none
    matches. Never returns a bare "newest file" — that would silently score
    the wrong adapter (the exact hazard test-references-production-symbol
    and verify-before-you-claim both warn about: never infer identity from
    recency alone).

    `pattern` defaults to ~/.human/logs/candidate-authorship-*.json,
    expanded at CALL time (not import time) so a subprocess invoked with an
    overridden $HOME resolves against that HOME, not whatever HOME was set
    when this module was first imported.
    """
    pattern = pattern or os.path.expanduser("~/.human/logs/candidate-authorship-*.json")
    adapter_key = os.path.normpath(str(adapter_path)) if adapter_path else None
    matches = []
    for p in glob.glob(pattern):
        try:
            with open(p) as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(data, dict):
            continue
        candidate_adapter = data.get("candidate_adapter")
        if candidate_adapter is None:
            continue
        if os.path.normpath(str(candidate_adapter)) == adapter_key:
            matches.append((os.path.getmtime(p), p))
    if not matches:
        return None
    matches.sort()
    return matches[-1][1]


def build_parser():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--score-json",
                    help="score_candidate_offline.py output JSON (primary path, AC-2.1)")
    ap.add_argument("--candidate-json",
                    help="authorship_gap.py --out JSON for the candidate (secondary path)")
    ap.add_argument("--serving-json",
                    help="authorship_gap.py --out JSON for what is currently serving "
                         "(secondary path)")
    ap.add_argument("--min-gain", type=float, default=0.0)
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)

    if not args.score_json and not (args.candidate_json and args.serving_json):
        print("FATAL: pass --score-json, or both --candidate-json and --serving-json",
              file=sys.stderr)
        return 2

    try:
        if args.score_json:
            inputs = load_gate_inputs_from_score_json(args.score_json)
        else:
            inputs = load_gate_inputs_from_gap_jsons(args.candidate_json, args.serving_json)
    except SystemExit as e:
        print(str(e), file=sys.stderr)
        return 2

    verdict = decide_promotion(
        inputs["candidate_twin"], inputs["serving_twin"], inputs["floor"],
        min_gain=args.min_gain)
    # Visibility, not a second threshold: both sides' full gap dicts already
    # carry ceiling/twin/floor + ci95 + gap_closed_fraction (authorship_gap.py
    # computes gap_closed_fraction itself), so a human reviewing a borderline
    # PASS/BLOCK can see the CI overlap without this module inventing a
    # second undocumented boundary.
    verdict["candidate_gap_closed_fraction"] = (inputs.get("candidate_gap") or {}).get(
        "gap_closed_fraction")
    verdict["serving_gap_closed_fraction"] = (inputs.get("serving_gap") or {}).get(
        "gap_closed_fraction")
    print(json.dumps(verdict, indent=2))
    return 0 if verdict["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
