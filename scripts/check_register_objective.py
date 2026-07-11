#!/usr/bin/env python3
"""Objective register check for steering/overlay A/B trials.

The pairwise LLM formality judge measured AT CHANCE against this metric
(13/26 agreement on objectively-decisive trials, 2026-07-09), so A/B
verdicts on the formality dimension must not rely on the judge alone.
This scores each trial deterministically: capitalized sentence starts
minus slang density — the exact surface features hu_rules_formal
prescribes and hu_rules_casual prescribes against.

Usage:
  python3 scripts/check_register_objective.py data/steering_ab_results_formality-overlay.json
"""
import json
import math
import re
import sys

SLANG = re.compile(
    r"\b(lol|nah|lemme|gonna|wanna|dunno|u|ya|tbh|idk|hru|yeah|yep|hey|man|dude)\b")


def register_score(text):
    """Deterministic formality proxy in ~[-1, 1]: fraction of capitalized
    sentence starts minus slang-token density (per 10 words)."""
    sents = [s.strip() for s in re.split(r"[.!?]+", text) if s.strip()]
    if not sents:
        return 0.0
    cap = sum(1 for s in sents if s[0].isupper()) / len(sents)
    slang = len(SLANG.findall(text.lower())) / max(1, len(text.split()) / 10)
    return cap - slang


def wilson(wins, n, z=1.96):
    if n == 0:
        return 0.5, 0.5, 0.5
    p = wins / n
    denom = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / denom
    adj = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return p, centre - adj, centre + adj


def main(path):
    d = json.load(open(path))
    wins = ties = losses = 0
    judge_agree = judge_total = 0
    for t in d["trials"]:
        off_s = register_score(t["off"])
        arm_s = register_score(t["arm_a"])
        objectively_formal = arm_s > off_s + 0.05
        if objectively_formal:
            wins += 1
        elif arm_s < off_s - 0.05:
            losses += 1
        else:
            ties += 1
        if abs(arm_s - off_s) > 0.3:  # judge scored only where metric is decisive
            judge_total += 1
            if t["arm_a_wins_primary"] == objectively_formal:
                judge_agree += 1
    n = len(d["trials"])
    p, lo, hi = wilson(wins, n)
    print(f"objective register: arm_a more formal {wins}/{n} "
          f"({p*100:.1f}%, CI [{lo*100:.1f}%, {hi*100:.1f}%]), "
          f"tie {ties}, reversed {losses}")
    p, lo, hi = wilson(judge_agree, judge_total)
    print(f"judge agreement on decisive trials: {judge_agree}/{judge_total} "
          f"({p*100:.1f}%, CI [{lo*100:.1f}%, {hi*100:.1f}%])")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else
                  "data/steering_ab_results_formality-overlay.json"))
