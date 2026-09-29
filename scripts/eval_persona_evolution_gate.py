#!/usr/bin/env python3
"""eval_persona_evolution_gate.py — the directional-fidelity gate for gap #9
(docs/plans/2026-09-02-persona-evolution/spec.md §5).

Inputs
------
  --results   a results JSON written by scripts/eval_persona_evolution.py
              (the HUMAN before/after measurement; carries per-axis
              `delta` and `moved_beyond_ci`)
  --event     which event in that file to gate (must have status OK)
  --pre-gen   generated replies from BEFORE the event, on a fixed prompt set
  --post-gen  generated replies from AFTER the event, on the same prompt set

Generation files are matched by the sha256 of their `context` (the prompt),
never by position or id, so any two of these shapes pair up:
  - blind-A/B triples: [{"id", "context", "huuman_reply", ...}]
  - classifier trials: {"trials": [{"context", "ai_response", ...}]}

Verdict
-------
For every axis the human measurement moved beyond its CI (and that is not
excluded — `lowercase_start_rate` is excluded by default per spec §3b
point 2: it is a device/autocapitalisation effect), score the SAME axis on
the matched generated replies and compare the sign of (post − pre) with the
sign of the human delta. PASS iff every gated axis matches. A human delta of
exactly 0 never counts as a match. Fewer than --min-matched prompt pairs, or
an event that is not status OK, refuses with INSUFFICIENT_DATA and writes
nothing to --out (.claude/rules/no-number-without-a-measurement.md).

READ-ONLY. Never prints reply text; only counts, means, CIs and signs.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval_persona_evolution as epe  # noqa: E402

DEFAULT_MIN_MATCHED = 30
DEFAULT_EXCLUDE = ("lowercase_start_rate",)
EXCLUDE_REASON = {
    "lowercase_start_rate": "device/autocapitalisation artefact: 0.00->0.26->0.02->0.36 "
                            "week-to-week inside one store (spec §3b point 2)",
}


def _ctx_key(context) -> str:
    return hashlib.sha256(json.dumps(context, sort_keys=True).encode("utf-8")).hexdigest()


def load_generations(path: str) -> dict:
    """{context_sha256: generated_reply}. Raises ValueError on an unknown shape."""
    data = json.loads(Path(path).read_text())
    if isinstance(data, dict) and isinstance(data.get("trials"), list):
        recs, reply_key = data["trials"], "ai_response"
    elif isinstance(data, list):
        recs, reply_key = data, "huuman_reply"
    else:
        raise ValueError(f"{path}: unrecognized generation-file shape (need a triples list or {{'trials': [...]}})")
    out = {}
    for r in recs:
        if not isinstance(r, dict) or "context" not in r or reply_key not in r:
            raise ValueError(f"{path}: record without 'context'/'{reply_key}' -- unrecognized shape")
        reply = r[reply_key]
        if isinstance(reply, str) and reply.strip():
            out[_ctx_key(r["context"])] = reply.strip()
    return out


def match_generations(pre: dict, post: dict):
    """[(pre_reply, post_reply)] for every context present in both."""
    return [(pre[k], post[k]) for k in pre if k in post]


def run_gate(results_path: str, event: str, pairs, min_matched: int, exclude=DEFAULT_EXCLUDE,
             n_resamples: int = 2000, seed: int = 42) -> dict:
    results = json.loads(Path(results_path).read_text())
    ev = results.get("events", {}).get(event)
    out = {"event": event, "results": results_path, "n_matched": len(pairs),
           "min_matched": min_matched, "excluded_axes": sorted(set(exclude))}
    if not ev or ev.get("status") != "OK":
        out["status"] = "INSUFFICIENT_DATA"
        out["reason"] = f"human measurement for event '{event}' is not status OK in {results_path}"
        return out
    if len(pairs) < min_matched:
        out["status"] = "INSUFFICIENT_DATA"
        out["reason"] = f"only {len(pairs)} matched prompt pairs < min_matched={min_matched}"
        return out

    pre_agg = epe.aggregate_window([p for p, _ in pairs], n_resamples=n_resamples, seed=seed)
    post_agg = epe.aggregate_window([q for _, q in pairs], n_resamples=n_resamples, seed=seed)
    axes = {}
    for label, human in ev["axes"].items():
        if not human.get("moved_beyond_ci"):
            continue
        if label in exclude:
            continue
        hd = float(human["delta"])
        gp, gq = pre_agg["axes"][label], post_agg["axes"][label]
        gd = gq["mean"] - gp["mean"]
        human_sign = (hd > 0) - (hd < 0)
        gen_sign = (gd > 0) - (gd < 0)
        axes[label] = {
            "human_delta": hd,
            "gen_pre": gp, "gen_post": gq, "gen_delta": gd,
            "sign_match": bool(human_sign != 0 and human_sign == gen_sign),
        }
    out["axes"] = axes
    out["exclusion_reasons"] = {a: EXCLUDE_REASON.get(a, "excluded by --exclude-axis") for a in out["excluded_axes"]}
    out["status"] = "PASS" if axes and all(a["sign_match"] for a in axes.values()) else "FAIL"
    if not axes:
        out["status"] = "INSUFFICIENT_DATA"
        out["reason"] = "no gated axis: nothing moved beyond CI (or everything that moved is excluded)"
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--results", required=True)
    p.add_argument("--event", default="job")
    p.add_argument("--pre-gen", required=True)
    p.add_argument("--post-gen", required=True)
    p.add_argument("--min-matched", type=int, default=DEFAULT_MIN_MATCHED)
    p.add_argument("--exclude-axis", action="append", default=None,
                   help=f"axis label to report but not gate (default: {', '.join(DEFAULT_EXCLUDE)}; pass --exclude-axis '' to gate everything)")
    p.add_argument("--n-resamples", type=int, default=2000)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--out", default=None, help="write the verdict here on PASS/FAIL only, never on refusal")
    args = p.parse_args(argv)

    exclude = DEFAULT_EXCLUDE if args.exclude_axis is None else tuple(a for a in args.exclude_axis if a)
    pre = load_generations(args.pre_gen)
    post = load_generations(args.post_gen)
    pairs = match_generations(pre, post)
    verdict = run_gate(args.results, args.event, pairs, args.min_matched, exclude,
                       n_resamples=args.n_resamples, seed=args.seed)
    verdict["pre_gen"] = {"path": args.pre_gen, "n": len(pre)}
    verdict["post_gen"] = {"path": args.post_gen, "n": len(post)}
    print(json.dumps(verdict, indent=2))
    if verdict["status"] == "INSUFFICIENT_DATA":
        sys.stderr.write("REFUSED: " + verdict.get("reason", "") + "; wrote nothing to --out.\n")
        return 2
    if args.out:
        Path(args.out).write_text(json.dumps(verdict, indent=2) + "\n")
    return 0 if verdict["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
