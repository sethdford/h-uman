#!/usr/bin/env python3
"""
Multi-Turn Conversation Evaluation

Tests personality consistency, context awareness, and naturalness across
3-5 exchange conversations. Each scenario has a theme and escalation pattern.

Usage:
  python3 scripts/eval_multiturn.py --gateway
"""

import json
import os
import sys
import time
import urllib.parse
import urllib.request

PROJECT_ID = os.environ.get("GOOGLE_CLOUD_PROJECT", "johnb-2025")
EVAL_MODEL = "gemini-3.1-pro-preview"
GATEWAY_URL = os.environ.get("HU_GATEWAY_URL", "http://127.0.0.1:3002")

_adc_token_cache = {"token": None, "expires": 0}

def _get_adc_token():
    if _adc_token_cache["token"] and time.time() < _adc_token_cache["expires"] - 60:
        return _adc_token_cache["token"]
    creds_path = os.path.expanduser("~/.config/gcloud/application_default_credentials.json")
    if not os.path.exists(creds_path):
        return None
    with open(creds_path) as f:
        creds = json.load(f)
    payload = urllib.parse.urlencode({
        "client_id": creds["client_id"],
        "client_secret": creds["client_secret"],
        "refresh_token": creds["refresh_token"],
        "grant_type": "refresh_token",
    }).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token",
                                 data=payload, headers={"Content-Type": "application/x-www-form-urlencoded"})
    resp = urllib.request.urlopen(req, timeout=10)
    data = json.loads(resp.read())
    _adc_token_cache["token"] = data["access_token"]
    _adc_token_cache["expires"] = time.time() + data.get("expires_in", 3600)
    return data["access_token"]

def _gemini_url():
    return (f"https://aiplatform.googleapis.com/v1/projects/{PROJECT_ID}/locations/global/"
            f"publishers/google/models/{EVAL_MODEL}:generateContent")

def call_gemini(prompt, temperature=0.3):
    payload = json.dumps({
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
        "generationConfig": {"temperature": temperature, "maxOutputTokens": 4096,
                             "responseMimeType": "application/json"}
    }).encode()
    token = _get_adc_token()
    if not token:
        raise RuntimeError("No ADC credentials found")
    headers = {"Content-Type": "application/json", "Authorization": f"Bearer {token}"}
    req = urllib.request.Request(_gemini_url(), data=payload, headers=headers)
    resp = urllib.request.urlopen(req, timeout=30)
    data = json.loads(resp.read())
    return data["candidates"][0]["content"]["parts"][0]["text"]

def get_ai_response(messages):
    payload = json.dumps({
        "model": "default",
        "messages": messages,
        "temperature": 0.9,
    }).encode()
    req = urllib.request.Request(f"{GATEWAY_URL}/v1/chat/completions",
                                 data=payload,
                                 headers={"Content-Type": "application/json"})
    resp = urllib.request.urlopen(req, timeout=30)
    data = json.loads(resp.read())
    return data["choices"][0]["message"]["content"]

SCENARIOS = [
    {
        "name": "casual_catchup",
        "description": "Casual friend catching up, escalates to plans",
        "turns": [
            "hey whats up",
            "not much, just bored. you doing anything this weekend?",
            "nice, wanna grab food saturday?",
            "cool, where should we go",
        ]
    },
    {
        "name": "emotional_escalation",
        "description": "Starts light, escalates to emotional vulnerability",
        "turns": [
            "hey",
            "so I've been thinking about stuff",
            "I don't know... like is this all there is? work eat sleep repeat",
            "yeah... I just feel stuck lately",
        ]
    },
    {
        "name": "debate_opinions",
        "description": "Friendly disagreement testing opinion-having",
        "turns": [
            "hot take: remote work is overrated",
            "idk man I think most people just slack off at home",
            "okay but what about all the studies showing productivity goes up",
            "lol agree to disagree I guess",
        ]
    },
    {
        "name": "banter_humor",
        "description": "Pure banter and roasting between friends",
        "turns": [
            "bro I just ran a 5k",
            "lol shut up that was like a warmup",
            "okay grandpa, when's the last time YOU worked out",
            "hahaha fair enough. but seriously good job",
        ]
    },
    {
        "name": "news_reaction_chain",
        "description": "Reacting to escalating news",
        "turns": [
            "dude guess what",
            "I got the promotion!!",
            "thanks! and they're giving me a team to lead",
            "I know right? drinks on me this weekend",
        ]
    },
    {
        "name": "advice_seeking",
        "description": "Friend asking for genuine advice",
        "turns": [
            "hey can I ask you something",
            "should I take a job that pays more but I'd hate the work",
            "yeah but like 40% more money",
            "what would you do",
        ]
    },
]

EVAL_DIMENSIONS = [
    "personality_consistency",
    "context_awareness",
    "humor_naturalness",
    "opinion_strength",
    "emotional_calibration",
    "message_length_appropriateness",
    "conversation_flow",
]

def evaluate_conversation(scenario_name, exchanges):
    conversation_text = ""
    for i, (user_msg, ai_msg) in enumerate(exchanges):
        conversation_text += f"  Turn {i+1}:\n"
        conversation_text += f"    Friend: {user_msg}\n"
        conversation_text += f"    Seth: {ai_msg}\n\n"

    prompt = f"""You are evaluating a multi-turn text message conversation for human authenticity.
The person named "Seth" is being evaluated. Rate the FULL conversation on these dimensions:

{json.dumps(EVAL_DIMENSIONS)}

For each dimension, score 1-10 and explain briefly.
Also give an overall_score (1-10) and overall_verdict: "HUMAN", "BORDERLINE", or "AI".

Key things to evaluate across the full conversation:
- Does Seth maintain a consistent personality across all turns?
- Does he reference earlier parts of the conversation naturally?
- Does his humor feel forced or natural?
- Does he have opinions or dodge them?
- Does his emotional tone calibrate to the conversation's mood?
- Are his messages appropriately short for texting?
- Does the conversation flow naturally or feel like isolated Q&A?

Conversation ({scenario_name}):
{conversation_text}

Return JSON with this structure:
{{"dimensions": {{"personality_consistency": {{"score": N, "note": "..."}}, ...}}, "overall_score": N, "overall_verdict": "HUMAN|BORDERLINE|AI", "reasoning": "..."}}"""

    try:
        raw = call_gemini(prompt)
        raw = raw.strip()
        if raw.startswith("```"):
            raw = raw.split("\n", 1)[1].rsplit("```", 1)[0]
        result = json.loads(raw)
        # 2026-09-04: the judge sometimes wraps the object in a one-element
        # list; every caller does result.get(...), and the nightly crashed on
        # 'list' object has no attribute 'get' before writing any verdict.
        if isinstance(result, list):
            result = next((r for r in result if isinstance(r, dict)), None)
        return result if isinstance(result, dict) else None
    except Exception as e:
        print(f"  Judge error: {e}")
        return None

def main():
    print("=" * 70)
    print("  MULTI-TURN CONVERSATION EVALUATION")
    print("=" * 70)
    print(f"  Scenarios: {len(SCENARIOS)}")
    print(f"  Judge: Gemini 3.1 Pro (independent)")
    print(f"  Gateway: {GATEWAY_URL}")
    print("=" * 70)

    all_results = []
    dim_totals = {d: [] for d in EVAL_DIMENSIONS}

    for si, scenario in enumerate(SCENARIOS):
        print(f"\n{'─' * 70}")
        print(f"  [{si+1}/{len(SCENARIOS)}] {scenario['name']}: {scenario['description']}")
        print(f"{'─' * 70}")

        messages = []
        exchanges = []

        for ti, user_msg in enumerate(scenario["turns"]):
            messages.append({"role": "user", "content": user_msg})
            try:
                ai_response = get_ai_response(messages)
            except Exception as e:
                ai_response = f"[ERROR: {e}]"
            messages.append({"role": "assistant", "content": ai_response})
            exchanges.append((user_msg, ai_response))
            print(f"  [{ti+1}] Friend: {user_msg}")
            print(f"       Seth: {ai_response}")

        result = evaluate_conversation(scenario["name"], exchanges)
        if result:
            verdict = result.get("overall_verdict", "?")
            score = result.get("overall_score", 0)
            print(f"\n  Score: {score}/10 | Verdict: {verdict}")
            print(f"  Reasoning: {result.get('reasoning', '')[:120]}")

            for dim in EVAL_DIMENSIONS:
                dim_data = result.get("dimensions", {}).get(dim, {})
                if isinstance(dim_data, dict) and "score" in dim_data:
                    dim_totals[dim].append(dim_data["score"])

            all_results.append({
                "scenario": scenario["name"],
                "exchanges": [{"user": u, "seth": s} for u, s in exchanges],
                "evaluation": result
            })

    print(f"\n{'=' * 70}")
    print("  MULTI-TURN SCORECARD")
    print(f"{'=' * 70}\n")

    verdicts = {"HUMAN": 0, "BORDERLINE": 0, "AI": 0}
    overall_scores = []
    for r in all_results:
        v = r["evaluation"].get("overall_verdict", "?")
        verdicts[v] = verdicts.get(v, 0) + 1
        overall_scores.append(r["evaluation"].get("overall_score", 0))

    print(f"  Verdicts: {verdicts.get('HUMAN', 0)} HUMAN / "
          f"{verdicts.get('BORDERLINE', 0)} BORDERLINE / "
          f"{verdicts.get('AI', 0)} AI\n")

    print(f"  {'Dimension':<35} {'Avg Score':>10}")
    print(f"  {'─' * 45}")
    for dim in sorted(EVAL_DIMENSIONS):
        scores = dim_totals.get(dim, [])
        if scores:
            avg = sum(scores) / len(scores)
            bar = "█" * int(avg) + "░" * (10 - int(avg))
            print(f"  {dim:<35} {avg:>5.1f}/10  {bar}")

    if overall_scores:
        overall_avg = sum(overall_scores) / len(overall_scores)
        print(f"\n  OVERALL MULTI-TURN FIDELITY       {overall_avg:.1f}/10")
        print(f"  {'=' * 45}")
        if overall_avg >= 8.0:
            print("  VERDICT: HUMAN — consistent personality across conversations")
        elif overall_avg >= 6.0:
            print("  VERDICT: BORDERLINE — mostly human with some consistency gaps")
        else:
            print("  VERDICT: AI — detectable patterns across conversation turns")

    results_path = os.path.join(os.path.dirname(__file__), "..", "data", "eval_multiturn.json")
    os.makedirs(os.path.dirname(results_path), exist_ok=True)
    with open(results_path, "w") as f:
        json.dump(all_results, f, indent=2)
    print(f"\n  Full results saved to {results_path}")


if __name__ == "__main__":
    main()
