#!/usr/bin/env python3
"""Score a preference-mode blind rating sheet: "which reply would you rather
RECEIVE from Seth" -- not "which one IS Seth" (see make_rating_sheet.py
--mode preference and PROTOCOL.md's "Preference measurement" section).

This is a SEPARATE, non-promotion-gating measurement
(sprints/sprint-better-than-human-2026-09-05/designs/US-6.md, US-6). It
reuses score.py's wilson()/score_rows() math UNMODIFIED (AC-6.2): "win rate"
is exactly the same binomial-proportion computation as "detection rate",
against a key whose value means "which side is the MODEL reply" instead of
"which side is the REAL Seth reply" -- make_rating_sheet.py --mode preference
writes that different key, stamped with a top-level "_mode": "preference"
marker this script requires before it will score anything.

This script NEVER writes ~/.human/blind_ab_gate.json or
docs/evaluation/blind_ab_gate.json -- see designs/US-6.md refusal condition
#5. The only file it ever writes is the aggregate evidence JSON
({n, win_rate, ci_lo, ci_hi, rater, date}), and only for --rater human with
n >= --min-n (default 20, AC-6.5's floor). No raw sheet text, no message
content, no phone numbers, no names are ever written by this script.

Usage:
    python3 score_preference.py sheet_alice.csv sheet_bob.csv --key answer_key.json \
        --rater human --evidence-out ../../sprints/.../evidence/US-6/preference-results-<date>.json
    python3 score_preference.py sheet.csv --key answer_key.json   # score-only, no write
    python3 score_preference.py --selftest
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from score import score_rows, wilson, load_sheets, detect_rater_kind  # unmodified imports (AC-6.2)

MIN_N = 20   # AC-6.5's own floor for "at least one real run"


def load_preference_key(path):
    """Load answer_key.json and verify it was built in preference mode.

    Refuses (raises ValueError) a key lacking the "_mode": "preference"
    marker make_rating_sheet.py --mode preference writes. Scoring a
    detection-mode key here would silently report a meaningless "win rate"
    over "which side is really Seth" -- the exact sheet/protocol conflation
    designs/US-6.md's Risks section calls out. Fail loud, per
    .claude/rules/ground-truth-over-proxy-signals.md's spirit of never
    trusting an unverified artifact.
    """
    with open(path) as f:
        raw = json.load(f)
    if not isinstance(raw, dict) or raw.get("_mode") != "preference":
        raise ValueError(
            f"{path} is not a preference-mode answer key (missing "
            f"'_mode': 'preference' -- was this built with "
            f"make_rating_sheet.py --mode preference?)")
    return {k: v for k, v in raw.items() if k != "_mode"}


def report(agg):
    print(f"items scored      : {agg['n']}")
    print(f"win rate          : {agg['detect']:.3f}   (>0.5 = model reply preferred over Seth's)")
    print(f"  95% Wilson CI   : [{agg['ci_lo']:.3f}, {agg['ci_hi']:.3f}]")
    print("per-rater win rate:")
    for k, (rate, t) in sorted(agg["per_rater"].items()):
        print(f"  {k:12} {rate:.3f}  (n={t})")


def selftest():
    # win_rate/CI must be byte-identical to calling wilson()/score_rows()
    # directly -- this script must never fork that math (AC-6.2).
    key = {f"t{i}": ("A" if i % 2 else "B") for i in range(20)}
    rows = [{"id": f"t{i}", "choice": key[f"t{i}"], "confidence": 3}
            for i in range(15)]  # 15/20 answered, all "correct" (model preferred)
    agg = score_rows(rows, key)
    assert agg["n"] == 15, agg["n"]
    expected = wilson(15, 15)
    assert abs(agg["detect"] - expected[0]) < 1e-12
    assert abs(agg["ci_lo"] - expected[1]) < 1e-12
    assert abs(agg["ci_hi"] - expected[2]) < 1e-12
    # n==0 must never be representable as a measurement.
    empty_agg = score_rows([], key)
    assert empty_agg["n"] == 0
    assert wilson(0, 0) == (0.0, 0.0, 0.0)
    print("selftest OK: win-rate math delegates to score.py unmodified, "
          "n=0 stays non-representable")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheets", nargs="*")
    ap.add_argument("--key")
    ap.add_argument("--rater", choices=("human", "synthetic"), default=None,
                     help="Who produced the ratings. An evidence JSON is written "
                          "ONLY for --rater human (AC-6.3) -- this measurement is "
                          "never promotion-gating and NEVER touches "
                          "~/.human/blind_ab_gate.json regardless of --rater.")
    ap.add_argument("--evidence-out", default=None,
                     help="Path to write the aggregate evidence JSON "
                          "({n, win_rate, ci_lo, ci_hi, rater, date}) -- "
                          "written only when --rater human and n >= --min-n.")
    ap.add_argument("--min-n", type=int, default=MIN_N,
                     help=f"Refuse (exit non-zero, write nothing) below this many "
                          f"scored pairs. Default {MIN_N} (AC-6.5's floor).")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest(); return
    if not a.sheets or not a.key:
        print("need sheets + --key (or --selftest)", file=sys.stderr); sys.exit(2)

    try:
        key = load_preference_key(a.key)
    except ValueError as e:
        print(f"RESULT_blind_ab_preference=INVALID ({e})", file=sys.stderr)
        sys.exit(2)

    rows = load_sheets(a.sheets)

    if a.rater == "human" and detect_rater_kind(rows) == "synthetic":
        print("--rater human refused: these rows carry the judge_api/judge_model "
              "stamps synthetic_judge.py writes -- an LLM judged this sheet. "
              "Provenance beats the claim (fail safe); re-run with --rater "
              "synthetic, or strip the stamp columns if a human genuinely "
              "re-rated it.", file=sys.stderr)
        sys.exit(2)

    agg = score_rows(rows, key)   # unmodified score.py math (AC-6.2)

    # Belt-and-suspenders: n==0 is also caught by the n<min-n check below,
    # but is checked explicitly first because it is the exact shape of the
    # 2026-07-25 vacuous-PASS incident
    # (.claude/rules/no-number-without-a-measurement.md) -- wilson(0, 0)
    # returns a well-formed (0.0, 0.0, 0.0), which must never be printed or
    # written as if it were a measurement.
    if agg["n"] == 0:
        print("RESULT_blind_ab_preference=INVALID (n=0 -- no choices matched "
              "the key; refusing to emit any verdict or evidence file)",
              file=sys.stderr)
        sys.exit(3)

    if agg["n"] < a.min_n:
        print(f"RESULT_blind_ab_preference=INVALID (n={agg['n']} < {a.min_n})",
              file=sys.stderr)
        sys.exit(3)

    report(agg)
    print(f"\nRESULT_blind_ab_preference=SCORED n={agg['n']} "
          f"win_rate={agg['detect']:.3f} ci=[{agg['ci_lo']:.3f},{agg['ci_hi']:.3f}]")

    if not a.evidence_out:
        print("\nNo --evidence-out given: nothing written (scoring-only run).")
        return

    if a.rater != "human":
        print(f"\n--rater {a.rater!r} != 'human': evidence file NOT written "
              f"(AC-6.3 -- only human-rated sheets produce a committed "
              f"evidence artifact).", file=sys.stderr)
        return

    evidence = {
        "n": agg["n"],
        "win_rate": round(agg["detect"], 4),
        "ci_lo": round(agg["ci_lo"], 4),
        "ci_hi": round(agg["ci_hi"], 4),
        "rater": "human",
        "date": time.strftime("%Y-%m-%d"),
    }
    out_dir = os.path.dirname(a.evidence_out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(a.evidence_out, "w") as f:
        json.dump(evidence, f, indent=2)
    print(f"\nwrote {a.evidence_out}")


if __name__ == "__main__":
    main()
