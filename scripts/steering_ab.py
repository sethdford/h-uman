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




# 20-question capability quiz — ensures steering doesn't degrade reasoning/recall
CAPABILITY_QUIZ = [
    {"q": "What is the capital of France?", "opts": ["London", "Paris", "Berlin", "Rome"], "ans": "B"},
    {"q": "What is 7 × 8?", "opts": ["54", "56", "58", "60"], "ans": "B"},
    {"q": "What does DNA stand for?", "opts": ["Deoxyribonucleic Acid", "Dynamic Nuclear Assembly", "Deoxyribose Nucleotide Array", "Diatomic Nitrogen Acceptor"], "ans": "A"},
    {"q": "Who wrote Romeo and Juliet?", "opts": ["Jane Austen", "William Shakespeare", "John Milton", "Christopher Marlowe"], "ans": "B"},
    {"q": "What is the chemical symbol for gold?", "opts": ["Go", "Gd", "Au", "Ag"], "ans": "C"},
    {"q": "Which planet is closest to the sun?", "opts": ["Venus", "Mercury", "Mars", "Earth"], "ans": "B"},
    {"q": "What is the square root of 144?", "opts": ["10", "11", "12", "13"], "ans": "C"},
    {"q": "In what year did World War II end?", "opts": ["1943", "1944", "1945", "1946"], "ans": "C"},
    {"q": "What is the largest ocean on Earth?", "opts": ["Atlantic", "Indian", "Arctic", "Pacific"], "ans": "D"},
    {"q": "What is the atomic number of Carbon?", "opts": ["4", "6", "8", "12"], "ans": "B"},
    {"q": "Which continent is Egypt in?", "opts": ["Asia", "Africa", "Europe", "Australia"], "ans": "B"},
    {"q": "What is the speed of light approximately?", "opts": ["300,000 km/s", "150,000 km/s", "500,000 km/s", "1,000,000 km/s"], "ans": "A"},
    {"q": "Who painted the Mona Lisa?", "opts": ["Michelangelo", "Leonardo da Vinci", "Raphael", "Donatello"], "ans": "B"},
    {"q": "What is the largest country by area?", "opts": ["Canada", "China", "Russia", "USA"], "ans": "C"},
    {"q": "What is 15 + 28?", "opts": ["41", "42", "43", "44"], "ans": "C"},
    {"q": "What is the main gas in the Earth's atmosphere?", "opts": ["Oxygen", "Carbon dioxide", "Nitrogen", "Argon"], "ans": "C"},
    {"q": "What does HTML stand for?", "opts": ["Hypertext Markup Language", "High Tech Modern Layout", "Home Tool Markup Language", "Hyperlink Typed Markup Language"], "ans": "A"},
    {"q": "What is the capital of Japan?", "opts": ["Osaka", "Tokyo", "Kyoto", "Hiroshima"], "ans": "B"},
    {"q": "Which vitamin is produced by sun exposure?", "opts": ["Vitamin A", "Vitamin B", "Vitamin C", "Vitamin D"], "ans": "D"},
    {"q": "What is the smallest prime number?", "opts": ["0", "1", "2", "3"], "ans": "C"},
]

def gen_reply(incoming, steering_dose, trait="warmth", timeout=180):
    """Generate a single reply with optional steering vector applied.

    Args:
        incoming: user message
        steering_dose: float in [-1, 1] or None for OFF (baseline)
        trait: steering vector name the server should apply ("warmth", "formality", ...)
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
        if steering_dose is not None:
            payload["steering"] = {trait: steering_dose}

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


def judge_reply_pair(incoming, arm_a_reply, arm_b_reply, experiment_type="warmth"):
    """Ask judge to compare two replies. Experiment type determines questions.

    Args:
        incoming: user message
        arm_a_reply: first reply
        arm_b_reply: second reply
        experiment_type: "warmth", "professional", or "formality" (determines judge questions)

    Returns dict with keys depending on experiment_type.
    """
    try:
        if experiment_type == "formality":
            # Formality context: ask about register + humanness
            prompt = f"""You are analyzing two text message responses for formality and humanness.

CONTEXT: Someone sent this message:
"{incoming}"

Two AI responses were generated.

RESPONSE A: "{arm_a_reply}"
RESPONSE B: "{arm_b_reply}"

Answer TWO questions in JSON:

1. **Formality**: Which response is more formal - more polished, professional register, complete sentences, no slang or texting shorthand? (A or B)
2. **Humanness**: Which response sounds more like a real human texting? (A or B)

Return ONLY this exact JSON (no markdown, no explanation):
{{
  "more_formal": "A" or "B",
  "humanness": "A" or "B",
  "reasoning": "one sentence explaining your formality choice"
}}"""
        elif experiment_type == "professional":
            # Professional context: ask about appropriateness + humanness
            prompt = f"""You are analyzing two text message responses for professionalism and humanness.

CONTEXT: A professional acquaintance sent this message:
"{incoming}"

Two AI responses were generated.

RESPONSE A: "{arm_a_reply}"
RESPONSE B: "{arm_b_reply}"

Answer TWO questions in JSON:

1. **Appropriateness**: Which response is more appropriate for a professional acquaintance (not a close friend)? (A or B)
2. **Humanness**: Which response sounds more like a real human texting? (A or B)

Return ONLY this exact JSON (no markdown, no explanation):
{{
  "appropriate": "A" or "B",
  "humanness": "A" or "B",
  "reasoning": "one sentence explaining your appropriateness choice"
}}"""
        else:  # warmth (default)
            # Warmth context: ask about warmth + humanness
            prompt = f"""You are analyzing two text message responses for warmth and humanness.

CONTEXT: A friend sent this message:
"{incoming}"

Two AI responses were generated.

RESPONSE A: "{arm_a_reply}"
RESPONSE B: "{arm_b_reply}"

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
            if experiment_type == "formality":
                return {
                    "more_formal": data.get("more_formal"),
                    "humanness": data.get("humanness"),
                    "reasoning": data.get("reasoning", ""),
                }
            elif experiment_type == "professional":
                return {
                    "appropriate": data.get("appropriate"),
                    "humanness": data.get("humanness"),
                    "reasoning": data.get("reasoning", ""),
                }
            else:
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




def run_capability_check(steered_dose, trait="warmth"):
    """Run 20-question quiz on both OFF and steered arms. Return (off_score, steered_score, passed)."""
    print(f"\n=== CAPABILITY CHECK (20 questions) ===")
    print(f"Testing: OFF vs {trait}@{steered_dose:+.1f}")
    print()

    off_correct = 0
    steered_correct = 0
    
    for i, q_data in enumerate(CAPABILITY_QUIZ):
        q = q_data["q"]
        opts = q_data["opts"]
        correct_letter = q_data["ans"]
        correct_idx = ord(correct_letter) - ord('A')
        
        # Generate answers for both arms
        off_reply, _ = gen_reply(q, None, trait=trait, timeout=180)
        steered_reply, _ = gen_reply(q, steered_dose, trait=trait, timeout=180)
        
        if not off_reply or not steered_reply:
            print(f"[{i+1}/20] gen-fail (off={bool(off_reply)} steered={bool(steered_reply)})")
            continue
        
        # Extract answer letter from reply (look for single A/B/C/D letter surrounded by non-letter)
        import re
        off_match = re.search(r'\b([A-D])\b', off_reply.upper())
        steered_match = re.search(r'\b([A-D])\b', steered_reply.upper())
        
        off_letter = off_match.group(1) if off_match else None
        steered_letter = steered_match.group(1) if steered_match else None
        
        off_correct_here = (off_letter == correct_letter)
        steered_correct_here = (steered_letter == correct_letter)
        
        if off_correct_here:
            off_correct += 1
        if steered_correct_here:
            steered_correct += 1
        
        status = f"OFF={'✓' if off_correct_here else '✗'} STEERED={'✓' if steered_correct_here else '✗'}"
        print(f"[{i+1}/20] {status}")
    
    off_pct = (off_correct / 20) * 100
    steered_pct = (steered_correct / 20) * 100
    delta = off_pct - steered_pct
    
    # FAIL criterion: steered accuracy >2 answers (10pp) below OFF
    passed = delta <= 10  # i.e., steered within 10pp of OFF
    
    print(f"\n  OFF accuracy: {off_correct}/20 ({off_pct:.0f}%)")
    print(f"  STEERED accuracy: {steered_correct}/20 ({steered_pct:.0f}%)")
    print(f"  Delta (OFF - STEERED): {delta:.0f}pp")
    print(f"  Threshold: ≤10pp")
    print(f"  Result: {'PASS' if passed else 'FAIL'} (steered {'within' if passed else 'OUTSIDE'} 10pp of OFF)")
    
    return off_correct, steered_correct, passed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument("--pilot", action="store_true", help="Run 2 contexts for quick validation")
    ap.add_argument("--experiment", choices=["warmth", "professional", "formality"],
                    default="warmth",
                    help="Experiment type: 'warmth' (WARM +dose vs OFF), 'professional' (COLD vs OFF "
                         "with professional context), or 'formality' (FORMAL +dose vs OFF, formality vector)")
    ap.add_argument("--dose", type=float, default=None,
                    help="Steering dose (float in [-1,1]). Defaults: warmth +0.4, professional -0.5, formality +0.6")
    ap.add_argument("--capability-check", action="store_true",
                    help="Run 20-question knowledge quiz on both arms to verify steering doesn't degrade reasoning")
    args = ap.parse_args()

    n = 2 if args.pilot else args.n

    # Determine the steering trait, dose and arm label
    steer_trait = "warmth"
    if args.experiment == "warmth":
        if args.dose is None:
            args.dose = 0.4  # Default subtle dose for warmth
        arm_label = f"WARM +{args.dose}"
        experiment_type_label = "warmth"
    elif args.experiment == "formality":
        steer_trait = "formality"
        if args.dose is None:
            args.dose = 0.6  # FORMAL direction of the formality vector
        arm_label = f"FORMAL +{args.dose}"
        experiment_type_label = "formality"
    else:  # professional
        if args.dose is None:
            args.dose = -0.5  # Default cold dose for professional
        arm_label = f"COLD {args.dose}"
        experiment_type_label = "professional"

    # Run capability check if requested
    if args.capability_check:
        off_correct, steered_correct, cap_passed = run_capability_check(args.dose, steer_trait)
        if not cap_passed:
            print(f"\n❌ CAPABILITY CHECK FAILED: steering degrades reasoning by >{10}pp")
            print(f"   Aborting A/B test.")
            return
        print(f"\n✓ Capability check passed; proceeding with A/B test")

    incomings = load_incomings(n)
    print(f"steering A/B round 2: n={len(incomings)} source=synthetic (embedded)")
    print(f"  experiment={args.experiment} (context={experiment_type_label})")
    print(f"  arm_a={arm_label} vs OFF")
    print()

    trials = []
    arm_a_wins = 0
    human_wins = 0
    errors = 0

    for i, inc in enumerate(incomings):
        t0 = time.time()

        # Generate two arms: OFF and steered
        off_reply, off_applied = gen_reply(inc, None, trait=steer_trait)
        arm_a_reply, arm_a_applied = gen_reply(inc, args.dose, trait=steer_trait)

        if not off_reply or not arm_a_reply:
            errors += 1
            print(f"[{i+1}/{len(incomings)}] gen-fail "
                  f"(off={bool(off_reply)} arm_a={bool(arm_a_reply)})")
            continue

        # Judge arm_a vs OFF
        judgment = judge_reply_pair(inc, arm_a_reply, off_reply, experiment_type=experiment_type_label)

        if not judgment:
            errors += 1
            print(f"[{i+1}/{len(incomings)}] judge-fail")
            continue

        # Randomize A/B order so judge doesn't position-bias
        arm_a_is_presented_as_a = (i % 2 == 0)
        a_reply = arm_a_reply if arm_a_is_presented_as_a else off_reply
        b_reply = off_reply if arm_a_is_presented_as_a else arm_a_reply

        # Map judgment back to arm names
        primary_key = {"warmth": "warmer", "professional": "appropriate",
                       "formality": "more_formal"}[experiment_type_label]
        arm_a_wins_primary = (judgment[primary_key] == "A" if arm_a_is_presented_as_a
                              else judgment[primary_key] == "B")
        arm_a_wins_human = (judgment["humanness"] == "A" if arm_a_is_presented_as_a
                            else judgment["humanness"] == "B")

        if arm_a_wins_primary:
            arm_a_wins += 1
        if arm_a_wins_human:
            human_wins += 1

        elapsed = round(time.time() - t0, 1)
        print(f"[{i+1}/{len(incomings)}] {primary_key}={arm_a_wins_primary} human={arm_a_wins_human} ({elapsed}s)")

        trials.append({
            "incoming": inc,
            "off": off_reply,
            "arm_a": arm_a_reply,
            "arm_a_dose": args.dose,
            "arm_a_is_presented_as_a": arm_a_is_presented_as_a,
            "judgment": judgment,
            "arm_a_wins_primary": arm_a_wins_primary,
            "arm_a_wins_human": arm_a_wins_human,
            "secs": elapsed,
        })

    decided = len(trials)
    primary_rate = (arm_a_wins / decided * 100) if decided else 0.0
    human_rate = (human_wins / decided * 100) if decided else 0.0

    _, primary_lower, primary_upper = wilson_ci(arm_a_wins, decided)
    primary_lower_pct = primary_lower * 100
    primary_upper_pct = primary_upper * 100

    # Determine pass criteria based on experiment type
    if args.experiment == "warmth":
        # For subtle warmth (+0.4), hypothesis: keep humanness >= 45%, warmth win > 50%
        pass_primary = primary_lower_pct > 50 and primary_rate >= 55
        pass_human = human_rate >= 45  # Stricter than round 1 (was 40%)
        primary_key_name = "warmth"
    elif args.experiment == "formality":
        # The vector must visibly shift register (formality wins decisively);
        # humanness is expected to dip (formal texting reads less human) but
        # should not collapse.
        pass_primary = primary_lower_pct > 50 and primary_rate >= 55
        pass_human = human_rate >= 40
        primary_key_name = "formality"
    else:  # professional
        # For professional context, we expect appropriateness to improve
        pass_primary = primary_lower_pct > 50 and primary_rate >= 55
        pass_human = human_rate >= 40
        primary_key_name = "appropriateness"

    verdict = "PASS" if (pass_primary and pass_human) else "INCONCLUSIVE"

    result = {
        "n_incomings": len(incomings),
        "decided": decided,
        "errors": errors,
        "source": "synthetic",
        "experiment": args.experiment,
        "arm_a_dose": args.dose,
        "arm_a_label": arm_label,
        f"{primary_key_name}_wins": arm_a_wins,
        f"{primary_key_name}_win_rate_pct": round(primary_rate, 1),
        f"{primary_key_name}_ci_lower_pct": round(primary_lower_pct, 1),
        f"{primary_key_name}_ci_upper_pct": round(primary_upper_pct, 1),
        "humanness_wins": human_wins,
        "humanness_win_rate_pct": round(human_rate, 1),
        "pass_criteria": {
            primary_key_name: pass_primary,
            "humanness": pass_human,
            "overall": verdict,
        },
        "trials": trials,
    }

    # Warmth keeps the original path; other experiments get their own file
    result_path = RESULT_PATH if args.experiment == "warmth" else \
        os.path.join(HERE, "..", "data", f"steering_ab_results_{args.experiment}.json")
    os.makedirs(os.path.dirname(result_path), exist_ok=True)
    with open(result_path, "w") as f:
        json.dump(result, f, indent=2)

    print(f"\n=== STEERING A/B RESULT: {args.experiment.upper()} ===")
    print(f"  {arm_label} wins ({primary_key_name}): {arm_a_wins}/{decided}")
    print(f"  {primary_key_name} win-rate: {primary_rate:.1f}%")
    print(f"  95% Wilson CI: [{primary_lower_pct:.1f}%, {primary_upper_pct:.1f}%]")
    print(f"  Humanness wins (arm_a): {human_wins}/{decided}")
    print(f"  Humanness win-rate: {human_rate:.1f}%")

    print(f"\n  {primary_key_name} check (CI lower > 50% AND win-rate >= 55%): {'PASS' if pass_primary else 'FAIL'}")
    print(f"  humanness check (win-rate >= {45 if args.experiment == 'warmth' else 40}%): {'PASS' if pass_human else 'FAIL'}")
    print(f"\n  OVERALL VERDICT: {verdict}")
    print(f"  written: {result_path}")

    # Write markdown verdict — read the existing content BEFORE opening for
    # write: open(..., "w") truncates immediately, so reading inside that
    # block always sees an empty file and silently discards prior rounds.
    os.makedirs(os.path.dirname(VERDICT_PATH), exist_ok=True)
    existing_content = ""
    if os.path.exists(VERDICT_PATH):
        with open(VERDICT_PATH, "r") as prev:
            existing_content = prev.read()
    with open(VERDICT_PATH, "w") as f:
        if args.experiment == "formality":
            round2_heading = f"## Formality Vector ({arm_label} vs OFF, n={decided})"
            hypothesis = (f"The formality vector ({args.dose:+.1f}) visibly shifts register toward "
                          f"formal (formality win-rate >50%) without collapsing humanness (≥40%)")
        else:
            round2_heading = f"## Round 2 ({args.experiment.capitalize()} Experiment, {arm_label} vs OFF)"
            hypothesis = ('Subtle dose (+0.4) maintains humanness ≥45% while warmth wins >50%'
                          if args.experiment == 'warmth' else
                          'Cold direction (-0.5) improves appropriateness in professional contexts while maintaining humanness')
        round2_section = f"""
{round2_heading}

**Hypothesis:** {hypothesis}

**Configuration:**
- MLX server: :8743 with persona adapter + steering vector loaded
- Arm A: {arm_label}
- Arm B: OFF (baseline)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n={decided} synthetic iMessage incomings
- Judge questions: {primary_key_name} (A or B) + humanness (A or B)

### Results

- **{primary_key_name.capitalize()} win-rate ({primary_key_name}): {primary_rate:.1f}%** ({arm_a_wins}/{decided})
- **95% Wilson CI: [{primary_lower_pct:.1f}%, {primary_upper_pct:.1f}%]**
- **Humanness win-rate: {human_rate:.1f}%** ({human_wins}/{decided})

### Pass Criteria

✓ **{primary_key_name.capitalize()}**: CI lower bound > 50% AND win-rate ≥ 55% → **{'PASS' if pass_primary else 'FAIL'}**
✓ **Humanness**: Win-rate ≥ {45 if args.experiment == 'warmth' else 40}% → **{'PASS' if pass_human else 'FAIL'}**

**Verdict: {verdict}**
"""
        if args.experiment != "formality":
            # Warmth round-1 comparison numbers only make sense for the warmth vector
            round2_section += f"""
### Dose-Response vs Round 1

| Metric | Round 1 (+1.0) | Round 2 ({args.dose}) | Direction |
|--------|---|---|---|
| {primary_key_name.capitalize()} | 50.0% | {primary_rate:.1f}% | {'+' if primary_rate > 50 else '-'} |
| Humanness | 35.0% | {human_rate:.1f}% | {'+' if human_rate > 35 else '-'} |
"""
        round2_section += """
### Example Triples (First 3)

"""

        for idx, trial in enumerate(trials[:3], start=1):
            round2_section += f"""**Context {idx}**

**Incoming:** "{trial['incoming']}"

**OFF (Baseline):**
{trial['off']}

**{arm_label}:**
{trial['arm_a']}

**Judge:** {trial['judgment']['reasoning']}
- {primary_key_name.capitalize()}: {trial['arm_a_wins_primary']}
- Humanness: {trial['arm_a_wins_human']}

"""

        # Write: preserve round 1 if present, append round 2
        if existing_content:
            f.write(existing_content)
            f.write("\n")
        f.write(round2_section)

    print(f"  verdict written: {VERDICT_PATH}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
