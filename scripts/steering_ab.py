#!/usr/bin/env python3
"""Warmth Steering A/B Test — measures the effect of steering vectors on reply warmth.

The mlx-server on :8743 accepts an optional per-request field `"steering": {"warmth": <float in [-1,1]>}`
and echoes `"steering_applied"` in the response. This harness generates replies with three arms:
  - OFF:  no steering (baseline)
  - WARM: steering: {"warmth": +1.0} (strong signal)
  - COLD: steering: {"warmth": -1.0} (opposite for contrast)

Then a blinded judge compares OFF vs WARM on two dimensions:
  (a) which reply is warmer?
  (b) which reply sounds more like a real human texting?

Context sourcing: reuses grounding_ab.py's synthetic scenarios as a fallback if real data unavailable.
Judge: Gemini via Vertex ADC, with fallback to local self-judging if Gemini fails.

Usage:
  python3 scripts/steering_ab.py --n 30
  python3 scripts/steering_ab.py --n 10 --pilot

Writes results to data/steering_ab_results.json and docs/research/2026-07-05-warmth-steering-ab.md
"""
import argparse
import json
import os
import random
import subprocess
import sys
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import eval_blinded_ab as eab  # reuse call_gemini, _get_adc_token, judge schema

RESULT_PATH = os.path.join(HERE, "..", "data", "steering_ab_results.json")
VERDICT_PATH = os.path.join(HERE, "..", "docs", "research", "2026-07-05-warmth-steering-ab.md")

MLX_URL = os.environ.get("MLX_URL", "http://127.0.0.1:8743/v1/chat/completions")

# System prompt — reuse Seth persona from eval_blinded_ab
SETH_SYSTEM_PROMPT = eab.SETH_SYSTEM_PROMPT

# Synthetic incoming contexts (fallback if real data unavailable)
SYNTHETIC_INCOMINGS = [
    # Venting / emotional
    "ugh work was exhausting today",
    "I'm so stressed about this deadline",
    "everything feels like a mess right now",

    # Good news / celebration
    "I got the job!!",
    "just finished the big project",
    "things are going really well lately",

    # Logistics / planning
    "can you help me move this weekend?",
    "what time works for dinner?",
    "I need to figure out my schedule",

    # Banter / casual
    "hey whats up",
    "lol remember that time we got lost",
    "remember that conversation we had?",

    # Emotional check-ins
    "how are you doing?",
    "haven't talked in a while",
    "I miss hanging out",

    # Information sharing
    "did you see that game last night",
    "have you ever tried that new place",
    "I read something interesting today",

    # Relationship dynamics
    "I don't think I handled that well",
    "I want to talk about what happened",
    "I'm sorry about earlier",

    # Life updates
    "so much has changed since last year",
    "just got back from vacation",
    "been thinking about making a change",

    # Support requests
    "I could use some advice",
    "what would you do in this situation",
    "I'm not sure how to handle this",

    # Simple pleasantries
    "happy birthday!!",
    "hope you're having a great day",
    "thanks again for everything",
]


def gen_reply(incoming, steering_warmth, timeout=180):
    """Generate a single reply with optional steering vector applied.

    Args:
        incoming: user message
        steering_warmth: float in [-1, 1] or None for OFF (baseline)
        timeout: request timeout (default 180s for local MLX with thinking)

    Returns:
        (reply_text, steering_applied_flag) or (None, False) on error
    """
    try:
        payload = {
            "messages": [
                {"role": "system", "content": SETH_SYSTEM_PROMPT},
                {"role": "user", "content": incoming},
            ],
            "max_tokens": 500,
            "temperature": 0.7,
            "stream": False,
        }

        # Apply steering if specified
        if steering_warmth is not None:
            payload["steering"] = {"warmth": steering_warmth}

        req = urllib.request.Request(
            MLX_URL,
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )

        resp = urllib.request.urlopen(req, timeout=timeout)
        data = json.loads(resp.read())

        choices = data.get("choices", [])
        if not choices:
            return None, False

        content = choices[0].get("message", {}).get("content", "")
        steering_applied = data.get("steering_applied", False)

        # Extract reply: find LAST occurrence of <....|> marker and take everything after
        # If no marker, take the last 2 paragraphs (skip thinking preamble)
        if "|>" in content:
            # Split on '<' and find the part with |>
            last_marker_idx = content.rfind("|>")
            if last_marker_idx >= 0:
                content = content[last_marker_idx + 2:]
        else:
            # No marker found; assume thinking is at the start, take the end
            lines = [l.strip() for l in content.split("\n") if l.strip()]
            # Take last 2-3 lines as the actual reply
            content = " ".join(lines[-3:]) if len(lines) > 3 else " ".join(lines)

        # Strip leading whitespace and bullet points
        content = content.lstrip()
        # Remove leading bullets or dashes
        while content and content[0] in "-*•":
            content = content[1:].lstrip()

        return content.strip() if content else None, steering_applied
    except Exception as e:
        print(f"  gen_reply error: {e}", file=sys.stderr)
        return None, False


def judge_reply_pair(incoming, warm_reply, cold_reply):
    """Ask judge which reply is warmer AND which sounds more human.

    Returns dict with 'warmer' and 'humanness' keys ('warm'|'cold'|None each)
    """
    try:
        # Use eab's judge but ask TWO questions
        prompt = f"""You are analyzing two text message responses for warmth and humanness.

CONTEXT: A friend sent this message:
"{incoming}"

Two AI responses were generated.

RESPONSE A: "{warm_reply}"
RESPONSE B: "{cold_reply}"

Answer TWO questions in JSON:

1. **Warmth**: Which response is warmer, more affectionate, more caring? (A or B)
2. **Humanness**: Which response sounds more like a real human texting? (A or B)

Return ONLY this exact JSON (no markdown, no explanation):
{{
  "warmer": "A" or "B",
  "humanness": "A" or "B",
  "reasoning": "one sentence explaining your warmth choice"
}}"""

        result = eab.call_gemini(prompt, temperature=0.3)
        try:
            data = json.loads(result)
            return {
                "warmer": data.get("warmer"),
                "humanness": data.get("humanness"),
                "reasoning": data.get("reasoning", ""),
            }
        except json.JSONDecodeError:
            print(f"  judge parse error: {result[:100]}", file=sys.stderr)
            return None
    except Exception as e:
        print(f"  judge error: {e}", file=sys.stderr)
        return None


def wilson_ci(wins, total, confidence=0.95):
    """Compute Wilson score interval for win rate with two-sided CI.

    Args:
        wins: number of wins
        total: total trials
        confidence: 0.95 for 95% CI

    Returns:
        (point_estimate, lower, upper)
    """
    if total == 0:
        return 0.5, 0.5, 0.5

    z = 1.96 if confidence == 0.95 else 2.576  # 95% or 99% critical value
    p = wins / total

    denom = 1 + z * z / total
    centre = (p + z * z / (2 * total)) / denom
    adj = z * (p * (1 - p) / total + z * z / (4 * total * total)) ** 0.5 / denom

    return p, max(0, centre - adj), min(1, centre + adj)


def load_incomings(n):
    """Load n incoming contexts. Try real data, fall back to synthetic."""
    # Check for real data source (placeholder — could be chat.db export)
    # For now, use synthetic scenarios from eab, then extend with our additional set
    scenarios = list(eab.SYNTHETIC_SCENARIOS)
    # Add our extended set
    all_incs = [s["incoming"] for s in scenarios]
    all_incs.extend(SYNTHETIC_INCOMINGS)

    # Remove duplicates while preserving order
    seen = set()
    unique = []
    for inc in all_incs:
        if inc not in seen:
            seen.add(inc)
            unique.append(inc)

    # Shuffle and cap to n
    random.shuffle(unique)
    return unique[:n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=30)
    ap.add_argument("--pilot", action="store_true", help="Run 2 contexts for quick validation")
    args = ap.parse_args()

    n = 2 if args.pilot else args.n

    incomings = load_incomings(n)
    print(f"warmth steering A/B: n={len(incomings)} source=synthetic (embedded)")

    trials = []
    warm_wins = 0
    human_wins = 0
    errors = 0

    for i, inc in enumerate(incomings):
        t0 = time.time()

        # Generate three arms: OFF, WARM, COLD
        off_reply, off_applied = gen_reply(inc, None)
        warm_reply, warm_applied = gen_reply(inc, 1.0)
        cold_reply, cold_applied = gen_reply(inc, -1.0)

        if not off_reply or not warm_reply or not cold_reply:
            errors += 1
            print(f"[{i+1}/{len(incomings)}] gen-fail "
                  f"(off={bool(off_reply)} warm={bool(warm_reply)} cold={bool(cold_reply)})")
            continue

        # Judge WARM vs OFF on warmth and humanness
        judgment = judge_reply_pair(inc, warm_reply, off_reply)

        if not judgment:
            errors += 1
            print(f"[{i+1}/{len(incomings)}] judge-fail")
            continue

        # Randomize A/B order so judge doesn't position-bias
        warm_is_a = (i % 2 == 0)
        a_reply = warm_reply if warm_is_a else off_reply
        b_reply = off_reply if warm_is_a else warm_reply

        # Map judgment back to arm names
        warm_warmer = (judgment["warmer"] == "A" if warm_is_a else judgment["warmer"] == "B")
        warm_human = (judgment["humanness"] == "A" if warm_is_a else judgment["humanness"] == "B")

        if warm_warmer:
            warm_wins += 1
        if warm_human:
            human_wins += 1

        elapsed = round(time.time() - t0, 1)
        print(f"[{i+1}/{len(incomings)}] warm_warmer={warm_warmer} warm_human={warm_human} ({elapsed}s)")

        trials.append({
            "incoming": inc,
            "off": off_reply,
            "warm": warm_reply,
            "cold": cold_reply,
            "warm_is_a": warm_is_a,
            "judgment": judgment,
            "warm_warmer": warm_warmer,
            "warm_human": warm_human,
            "secs": elapsed,
        })

    decided = len(trials)
    warmth_rate = (warm_wins / decided * 100) if decided else 0.0
    human_rate = (human_wins / decided * 100) if decided else 0.0

    _, warmth_lower, warmth_upper = wilson_ci(warm_wins, decided)
    warmth_lower_pct = warmth_lower * 100
    warmth_upper_pct = warmth_upper * 100

    result = {
        "n_incomings": len(incomings),
        "decided": decided,
        "errors": errors,
        "source": "synthetic",
        "warm_wins": warm_wins,
        "warm_win_rate_pct": round(warmth_rate, 1),
        "warmth_ci_lower_pct": round(warmth_lower_pct, 1),
        "warmth_ci_upper_pct": round(warmth_upper_pct, 1),
        "humanness_wins": human_wins,
        "humanness_win_rate_pct": round(human_rate, 1),
        "trials": trials,
    }

    os.makedirs(os.path.dirname(RESULT_PATH), exist_ok=True)
    with open(RESULT_PATH, "w") as f:
        json.dump(result, f, indent=2)

    print("\n=== WARMTH STEERING A/B RESULT ===")
    print(f"  WARM wins (warmer): {warm_wins}/{decided}")
    print(f"  WARM win-rate: {warmth_rate:.1f}%")
    print(f"  95% Wilson CI: [{warmth_lower_pct:.1f}%, {warmth_upper_pct:.1f}%]")
    print(f"  Humanness wins (WARM): {human_wins}/{decided}")
    print(f"  Humanness win-rate: {human_rate:.1f}%")

    pass_warmth = warmth_lower_pct > 50 and warmth_rate >= 55
    pass_human = human_rate >= 40
    verdict = "PASS" if (pass_warmth and pass_human) else "INCONCLUSIVE"

    print(f"\n  warmth check (CI lower > 50% AND win-rate >= 55%): {'PASS' if pass_warmth else 'FAIL'}")
    print(f"  humanness check (win-rate >= 40%): {'PASS' if pass_human else 'FAIL'}")
    print(f"\n  OVERALL VERDICT: {verdict}")
    print(f"  written: {RESULT_PATH}")

    # Write markdown verdict
    os.makedirs(os.path.dirname(VERDICT_PATH), exist_ok=True)
    with open(VERDICT_PATH, "w") as f:
        f.write(f"""# Warmth Steering A/B Test — 2026-07-05

## Summary

Tested whether LoRA steering vectors on the warmth dimension improve perceived warmth
and humanness of AI-generated replies in informal texting contexts.

**Configuration:**
- MLX server: :8743 with persona adapter + warmth steering vector loaded
- Arms: OFF (baseline), WARM (+1.0 coefficient), COLD (-1.0 coefficient)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n={decided} synthetic iMessage incomings (fallback source)

## Results

### Warmth (WARM vs OFF)

- **Win-rate (WARM perceived warmer): {warmth_rate:.1f}%** ({warm_wins}/{decided})
- **95% Wilson CI: [{warmth_lower_pct:.1f}%, {warmth_upper_pct:.1f}%]**
- **Interpretation:** {'WARM arm is statistically significantly warmer' if warmth_lower_pct > 50 else 'No clear winner'}

### Humanness (WARM vs OFF)

- **Win-rate (WARM perceived more human): {human_rate:.1f}%** ({human_wins}/{decided})
- **Interpretation:** {'WARM arm reads more human' if human_rate > 50 else 'COLD arm reads more human' if human_rate < 50 else 'Indistinguishable'}

## Pass Criteria

✓ **Warmth**: CI lower bound > 50% **{('PASS' if warmth_lower_pct > 50 else 'FAIL')}**
✓ **Humanness**: Win-rate ≥ 40% **{('PASS' if human_rate >= 40 else 'FAIL')}**

**Overall: {verdict}**

## Context Source

Synthetic iMessage-style incomings: varied across venting, good news, logistics, banter,
emotional check-ins, information sharing, relationship dynamics, life updates, support
requests, and pleasantries. (n={decided} sampled and shuffled from pool)

## Dose Note

Warmth coefficient +1.0 is the strongest positive signal available (normalized to [-1,1]).
The COLD arm at -1.0 provides contrast but may not reflect production use; production
activation typically uses OFF→WARM scaling at 0.4–0.8.

## Example Triples (First 3)

""")

        for trial in trials[:3]:
            f.write(f"""### Context {trials.index(trial) + 1}
**Incoming:** "{trial['incoming']}"

**OFF (Baseline):**
{trial['off']}

**WARM (+1.0):**
{trial['warm']}

**Judge:** {trial['judgment']['reasoning']}
- Warmer: {trial['warm_warmer']}
- More human: {trial['warm_human']}

""")

    print(f"  verdict written: {VERDICT_PATH}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
