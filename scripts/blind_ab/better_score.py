#!/usr/bin/env python3
"""Score the "better" measurement: is h-uman's reply judged BETTER than Seth's
real reply, not merely indistinguishable from it?

score.py's detection measurement answers "which sounds more like you?" --
0.5 detection is the goal (indistinguishable). This script answers a
DIFFERENT question, asked by rating_drip.py as a second pass over the SAME
items after the detection pass is answered: "which reply is BETTER for this
person?" A detection rate of 0.5 says nothing about whether the model's
reply is actually *better* -- a model could be indistinguishable AND worse
(or indistinguishable AND better). This is the only measurement that can
show "better than human," not just "as good as."

Reuses score.py's score_rows()/wilson() math UNMODIFIED, the same way
score_preference.py does for the (differently-sourced) preference
measurement -- one binomial-proportion implementation, never forked.
rating_drip.py composes the "better" question with a FRESH, independently
re-randomized A/B assignment (not the detection sheet's A/B) each time a
batch is sent, specifically so the owner cannot answer from memory of the
detection labeling. The mapping for that re-randomized assignment is
written by rating_drip.py to better_key.json (id -> "A"|"B", the displayed
letter holding h-uman's reply for THAT posing) -- see
rating_drip.decide_better_key().

This script NEVER writes ~/.human/blind_ab_gate.json or
docs/evaluation/blind_ab_gate.json (the LoRA promotion gate). Its only
output is the aggregate evidence JSON at --out (default
~/.human/blind_ab_better.json), written only when n > 0 -- a sheet with
zero answered items must never produce a well-formed rate (see
.claude/rules/no-number-without-a-measurement.md: wilson(0, 0) is a
well-formed (0.0, 0.0, 0.0) and must never be mistaken for a measurement).

Usage:
    python3 better_score.py rating_sheet.csv --better-key better_key.json
    python3 better_score.py sheet.csv --better-key better_key.json --out /tmp/better.json
    python3 better_score.py --selftest
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from score import score_rows, wilson, load_sheets, detect_rater_kind  # unmodified math

DEFAULT_OUT = os.path.expanduser("~/.human/blind_ab_better.json")
BETTER_FIELD = "better_choice"


def load_better_key(path):
    """better_key.json: {id: "A"|"B"} -- the displayed letter holding
    h-uman's (model) reply for that specific posing of the "better"
    question. Written by rating_drip.decide_better_key(); no "_mode"
    wrapper (unlike make_rating_sheet.py --mode preference's key) because
    this key is generated per-batch at send time, not at sheet build time."""
    with open(path) as f:
        raw = json.load(f)
    if not isinstance(raw, dict):
        raise ValueError(f"{path}: expected a JSON object of {{id: 'A'|'B'}}")
    return raw


def load_better_rows(paths):
    """Load sheet(s) and remap the better_choice column onto score_rows()'s
    expected 'choice' field, so the SAME win-rate math as score.py/
    score_preference.py scores this column too -- one implementation, never
    forked (mirrors score_preference.py's reuse of score.py)."""
    rows = load_sheets(paths)  # tags _rater; from score.py
    out = []
    for r in rows:
        out.append({
            "id": r.get("id"),
            "choice": (r.get(BETTER_FIELD) or "").strip(),
            "confidence": r.get("confidence") or 3,
            "_rater": r.get("_rater", "?"),
        })
    return out


def report(agg):
    print(f"items scored (better)  : {agg['n']}")
    print(f"better-than-human rate  : {agg['detect']:.3f}   "
          f"(0.50 = tied with Seth; >0.50 = h-uman judged better)")
    print(f"  95% Wilson CI         : [{agg['ci_lo']:.3f}, {agg['ci_hi']:.3f}]")
    print("per-rater better rate:")
    for k, (rate, t) in sorted(agg["per_rater"].items()):
        print(f"  {k:12} {rate:.3f}  (n={t})")


def selftest():
    # better-rate math must be byte-identical to calling wilson()/score_rows()
    # directly -- never fork the binomial-proportion implementation.
    key = {f"t{i}": ("A" if i % 2 else "B") for i in range(20)}
    rows = [{"id": f"t{i}", "choice": key[f"t{i}"], "confidence": 3, "_rater": "x"}
            for i in range(14)]  # 14/20 answered, all matching the model side
    agg = score_rows(rows, key)
    assert agg["n"] == 14, agg["n"]
    expected = wilson(14, 14)
    assert abs(agg["detect"] - expected[0]) < 1e-12
    assert abs(agg["ci_lo"] - expected[1]) < 1e-12

    # n==0 must never be representable as a measurement.
    assert score_rows([], key)["n"] == 0
    assert wilson(0, 0) == (0.0, 0.0, 0.0)

    # load_better_rows remaps better_choice -> choice without disturbing id.
    import tempfile, csv
    f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, newline="")
    w = csv.DictWriter(f, fieldnames=["id", "better_choice", "confidence"])
    w.writeheader()
    w.writerow({"id": "t0", "better_choice": "A", "confidence": ""})
    w.writerow({"id": "t1", "better_choice": "", "confidence": ""})  # unanswered
    f.close()
    try:
        rows = load_better_rows([f.name])
        assert rows[0]["id"] == "t0" and rows[0]["choice"] == "A"
        assert rows[1]["id"] == "t1" and rows[1]["choice"] == ""
        agg2 = score_rows(rows, {"t0": "A", "t1": "B"})
        assert agg2["n"] == 1, "the unanswered row must not be scored"
    finally:
        os.unlink(f.name)

    # Backward compatibility: a sheet with no better_choice column at all
    # (DictReader omits the key entirely) must score as n=0, not crash.
    f2 = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, newline="")
    w2 = csv.DictWriter(f2, fieldnames=["id", "choice", "confidence"])
    w2.writeheader()
    w2.writerow({"id": "t0", "choice": "A", "confidence": "3"})
    f2.close()
    try:
        rows = load_better_rows([f2.name])
        assert rows[0]["choice"] == "", "missing better_choice column -> empty, not an exception"
        agg3 = score_rows(rows, {"t0": "A"})
        assert agg3["n"] == 0
    finally:
        os.unlink(f2.name)

    print("selftest OK: better-rate math delegates to score.py unmodified, "
          "n=0 stays non-representable, no-better_choice sheets degrade safely")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheets", nargs="*")
    ap.add_argument("--better-key")
    ap.add_argument("--out", default=DEFAULT_OUT,
                     help="Where to write the better-than-human evidence JSON. "
                          f"Default {DEFAULT_OUT}. NEVER the LoRA promotion gate "
                          "file (~/.human/blind_ab_gate.json / "
                          "docs/evaluation/blind_ab_gate.json) -- this "
                          "measurement is never promotion-gating.")
    ap.add_argument("--arm-adapter", default=None)
    ap.add_argument("--arm-note", default=None)
    ap.add_argument("--dry-run", action="store_true",
                     help="score and print, but do not write --out")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest(); return
    if not a.sheets or not a.better_key:
        print("need sheets + --better-key (or --selftest)", file=sys.stderr)
        sys.exit(2)

    better_key = load_better_key(a.better_key)
    rows = load_better_rows(a.sheets)

    if detect_rater_kind(rows) == "synthetic":
        print("refused: these rows carry the judge_api/judge_model stamps "
              "synthetic_judge.py writes -- an LLM judged this sheet, not "
              "the owner. The better-than-human measurement is only "
              "meaningful as a real human preference.", file=sys.stderr)
        sys.exit(2)

    agg = score_rows(rows, better_key)

    # A sheet with ZERO scored items must never produce a rate: see
    # .claude/rules/no-number-without-a-measurement.md. wilson(0, 0) is a
    # well-formed (0.0, 0.0, 0.0) -- printing or writing it reads as "h-uman
    # never wins" from no evidence at all, not as "unmeasured."
    if agg["n"] == 0:
        print("RESULT_better=INVALID (0 items scored -- no better_choice "
              "answers matched the key; refusing to emit a rate or write "
              "a measurement)", file=sys.stderr)
        sys.exit(3)

    report(agg)
    print(f"\nRESULT_better=SCORED n={agg['n']} "
          f"better_rate={agg['detect']:.3f} ci=[{agg['ci_lo']:.3f},{agg['ci_hi']:.3f}]")

    if a.dry_run:
        print("\n--dry-run: nothing written.")
        sys.exit(0)

    out = {
        "schema_version": 1,
        "n": agg["n"],
        "better_rate": round(agg["detect"], 4),
        "ci_lo": round(agg["ci_lo"], 4),
        "ci_hi": round(agg["ci_hi"], 4),
        "weighted_better_rate": round(agg["weighted_detect"], 4),
        "updated_at": time.time(),
        "date": time.strftime("%Y-%m-%d"),
    }
    if a.arm_adapter or a.arm_note:
        out["arm"] = {k: v for k, v in (("adapter", a.arm_adapter),
                                         ("note", a.arm_note)) if v}
    out_dir = os.path.dirname(a.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(a.out, "w") as f:
        json.dump(out, f, indent=2)
    print(f"\nwrote {a.out}")
    sys.exit(0)


if __name__ == "__main__":
    main()
