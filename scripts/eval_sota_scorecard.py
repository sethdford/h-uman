#!/usr/bin/env python3
"""
SOTA scorecard generator for the 2026-05-18 persona-eval audit chain.

Reads all eval_runs from ~/.human/memory.db and produces a scorecard with:
- LLM-judge pass rate
- Deterministic shape-classifier pass rate
- Mean shape score
- Non-NULL response rate (separates "MLX dropped request" from "model misbehaved")

For each of the 4 relevant suites (imessage_humanness, tier1_naturalness,
humor_engine, human_likeness), produces a pre-fix-vs-post-fix delta where
applicable. Uses run_id ≤ 4 as PRE-FIX (the original NULL-system-prompt
runs) and run_id ≥ 5 as POST-FIX (the persona-system-prompt runs after
fix 33f8eaa5).

Bootstrap CI on shape-score per the Phase 5 eval-gate methodology
(but applied to shape-score not pass-rate, which is the metric the
2026-05-18 audit established is reliable).

Usage:
  python3 scripts/eval_sota_scorecard.py
  python3 scripts/eval_sota_scorecard.py --markdown   # render as markdown
"""

import argparse
import json
import random
import sqlite3
import statistics
import sys
from pathlib import Path

# Reuse the classifier from the sibling script
sys.path.insert(0, str(Path(__file__).parent))
from eval_shape_classifier import classify  # noqa: E402

DB_PATH = Path.home() / ".human" / "memory.db"
PRE_FIX_RUN_CUTOFF = 4  # runs with id <= 4 are pre-fix

# Suites we care about for the iMessage-style persona-fix evaluation.
# Fidelity / adversarial / capability_edges / etc. test orthogonal axes.
RELEVANT_SUITES = (
    "imessage-humanness",
    "tier1-naturalness",
    "humor-engine",
    "human-likeness",
)


def bootstrap_ci(values, n_resamples: int = 1000, confidence: float = 0.95, seed: int = 42):
    """Returns (mean, lower-CI, upper-CI). Light implementation matching
    src/eval/bootstrap_ci.c's contract."""
    if not values:
        return (0.0, 0.0, 0.0)
    if len(values) == 1:
        return (values[0], values[0], values[0])
    rng = random.Random(seed)
    n = len(values)
    resample_means = []
    for _ in range(n_resamples):
        sample = [values[rng.randrange(n)] for _ in range(n)]
        resample_means.append(sum(sample) / n)
    resample_means.sort()
    alpha = (1 - confidence) / 2
    lo = resample_means[int(alpha * n_resamples)]
    hi = resample_means[int((1 - alpha) * n_resamples)]
    return (statistics.mean(values), lo, hi)


def aggregate_run(con, run_id):
    rows = con.execute(
        "SELECT actual_output, passed FROM eval_results WHERE run_id=?", (run_id,)
    ).fetchall()
    if not rows:
        return None
    total = len(rows)
    shape_scores = [classify(r[0])["score"] for r in rows]
    shape_pass = sum(1 for r in rows if classify(r[0])["pass"])
    judge_pass = sum(1 for r in rows if r[1])
    non_null = sum(1 for r in rows if r[0])
    return {
        "n": total,
        "non_null": non_null,
        "judge_pass": judge_pass,
        "shape_pass": shape_pass,
        "shape_scores": shape_scores,
    }


def summarize_suite(con, suite_name, markdown=False):
    runs = con.execute(
        "SELECT id, created_at FROM eval_runs WHERE suite_name=? ORDER BY id",
        (suite_name,),
    ).fetchall()
    pre = []
    post = []
    for run_id, _ts in runs:
        agg = aggregate_run(con, run_id)
        if not agg:
            continue
        bucket = pre if run_id <= PRE_FIX_RUN_CUTOFF else post
        bucket.append((run_id, agg))

    def stats(bucket, label):
        if not bucket:
            return None
        all_scores = [s for _, agg in bucket for s in agg["shape_scores"]]
        m, lo, hi = bootstrap_ci(all_scores)
        n_runs = len(bucket)
        n_tasks = sum(agg["n"] for _, agg in bucket)
        non_null_pct = sum(agg["non_null"] for _, agg in bucket) / n_tasks
        judge_pct = sum(agg["judge_pass"] for _, agg in bucket) / n_tasks
        shape_pct = sum(agg["shape_pass"] for _, agg in bucket) / n_tasks
        return {
            "label": label,
            "n_runs": n_runs,
            "n_tasks": n_tasks,
            "non_null_pct": non_null_pct,
            "judge_pct": judge_pct,
            "shape_pct": shape_pct,
            "shape_mean": m,
            "shape_ci_lo": lo,
            "shape_ci_hi": hi,
        }

    s_pre = stats(pre, "PRE")
    s_post = stats(post, "POST")
    return {"suite": suite_name, "pre": s_pre, "post": s_post}


def render_table(summaries, markdown=False):
    if markdown:
        print("| Suite | n (pre/post) | Judge% pre→post | Shape% pre→post | Mean shape pre→post (95% CI) | Non-NULL% pre→post |")
        print("|-------|--------------|-----------------|-----------------|------------------------------|--------------------|")
        for s in summaries:
            suite = s["suite"]
            pre, post = s["pre"], s["post"]
            def fmt(field, scale):
                if pre is None and post is None:
                    return "no runs"
                if pre is None:
                    return f"— → {post[field]*scale:.1f}%"
                if post is None:
                    return f"{pre[field]*scale:.1f}% → —"
                return f"{pre[field]*scale:.1f}% → **{post[field]*scale:.1f}%**"
            def shape_ci():
                if pre is None and post is None:
                    return "—"
                if pre is None:
                    return f"— → **{post['shape_mean']:.3f}** [{post['shape_ci_lo']:.3f}, {post['shape_ci_hi']:.3f}]"
                if post is None:
                    return f"{pre['shape_mean']:.3f} [{pre['shape_ci_lo']:.3f}, {pre['shape_ci_hi']:.3f}] → —"
                return f"{pre['shape_mean']:.3f} → **{post['shape_mean']:.3f}** [{post['shape_ci_lo']:.3f}, {post['shape_ci_hi']:.3f}]"
            def n_runs():
                pn = pre["n_runs"] if pre else 0
                pon = post["n_runs"] if post else 0
                return f"{pn}/{pon}"
            print(f"| {suite} | {n_runs()} | {fmt('judge_pct', 100)} | {fmt('shape_pct', 100)} | {shape_ci()} | {fmt('non_null_pct', 100)} |")
    else:
        print(f"\n{'Suite':<22} {'n':>8} {'judge%':>10} {'shape%':>10} {'mean_shape':>11} {'non-NULL%':>10}")
        print("-" * 80)
        for s in summaries:
            suite = s["suite"]
            for state in ("pre", "post"):
                bs = s[state]
                if bs is None:
                    print(f"  {state:<5} {suite:<14} {'(no runs)':>40}")
                    continue
                ci = f"[{bs['shape_ci_lo']:.3f}, {bs['shape_ci_hi']:.3f}]"
                print(f"  {state:<5} {suite:<14} {bs['n_runs']}r/{bs['n_tasks']}t {100*bs['judge_pct']:>8.1f}% {100*bs['shape_pct']:>8.1f}% "
                      f"{bs['shape_mean']:>5.3f} {ci} {100*bs['non_null_pct']:>8.1f}%")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--markdown", action="store_true", help="render as markdown table")
    args = p.parse_args()

    con = sqlite3.connect(DB_PATH)
    summaries = []
    for suite in RELEVANT_SUITES:
        summaries.append(summarize_suite(con, suite))

    if args.markdown:
        print("\n# SOTA Scorecard — Persona Eval Fix (2026-05-18 audit chain)\n")
        print("**Pre-fix**: eval framework passed NULL system prompt (src/eval.c:572 before commit 33f8eaa5)")
        print("**Post-fix**: eval framework loads persona via hu_persona_build_prompt + threads through hu_eval_suite_t::system_prompt\n")
    render_table(summaries, markdown=args.markdown)

    # Headline finding
    print()
    impressive = []
    for s in summaries:
        pre, post = s["pre"], s["post"]
        if pre is None or post is None:
            continue
        delta_score = post["shape_mean"] - pre["shape_mean"]
        if delta_score > 0.1:
            impressive.append((s["suite"], delta_score, pre["shape_mean"], post["shape_mean"]))
    if impressive:
        print("HEADLINE FINDINGS:")
        for suite, delta, pre_m, post_m in impressive:
            relative = (delta / max(pre_m, 0.001)) * 100 if pre_m > 0 else float("inf")
            print(f"  {suite}: mean shape-score {pre_m:.3f} → {post_m:.3f} (+{delta:.3f}, {relative:+.0f}% relative)")


if __name__ == "__main__":
    main()
