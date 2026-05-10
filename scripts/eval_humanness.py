#!/usr/bin/env python3
"""
Synthetic Human-Fidelity Evaluation Harness

Sends diverse conversational prompts through `hu agent -m` with the full
persona + Phase 9 pipeline, then uses a separate Gemini call to score
each response on 12 human-behavior dimensions.

Produces a scorecard proving the system exhibits human-like behavior.
"""

import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request

API_KEY = os.environ.get("GEMINI_API_KEY", "")
PROJECT_ID = os.environ.get("GOOGLE_CLOUD_PROJECT", "johnb-2025")
EVAL_MODEL = "gemini-3.1-pro-preview"

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
    if API_KEY:
        return f"https://generativelanguage.googleapis.com/v1beta/models/{EVAL_MODEL}:generateContent?key={API_KEY}"
    return (f"https://aiplatform.googleapis.com/v1/projects/{PROJECT_ID}/locations/global/"
            f"publishers/google/models/{EVAL_MODEL}:generateContent")
HU_BIN = sys.argv[1] if len(sys.argv) > 1 else "hu"
GATEWAY_URL = os.environ.get("HU_GATEWAY_URL", "http://127.0.0.1:3002")
USE_GATEWAY = "--gateway" in sys.argv

SCENARIOS = [
    {
        "id": "casual_greeting",
        "prompt": "hey whats up",
        "context": "Casual text from a close friend at 11pm on a Friday",
        "expect": "informal, brief, possibly tired/relaxed tone"
    },
    {
        "id": "emotional_support",
        "prompt": "I'm having a really rough day. Work is crushing me and I feel like I can't keep up.",
        "context": "Close friend venting about stress",
        "expect": "empathetic, supportive, not overly verbose or clinical"
    },
    {
        "id": "exciting_news",
        "prompt": "DUDE I just got promoted!!! 🎉🎉🎉",
        "context": "Friend sharing exciting career news",
        "expect": "enthusiastic, celebratory, matching energy, possibly emojis"
    },
    {
        "id": "deep_question",
        "prompt": "Do you ever think about what you'd do if money wasn't a thing?",
        "context": "Late-night philosophical conversation with close friend",
        "expect": "thoughtful, personal, vulnerable, not a list of activities"
    },
    {
        "id": "mundane_logistics",
        "prompt": "What time works for dinner tomorrow?",
        "context": "Planning logistics with a friend",
        "expect": "brief, practical, possibly mentions own schedule constraints"
    },
    {
        "id": "disagreement",
        "prompt": "I think remote work is way overrated honestly. People are just lazy.",
        "context": "Friend sharing a controversial opinion",
        "expect": "pushback or nuance, not just agreement, shows own perspective"
    },
    {
        "id": "vulnerability",
        "prompt": "I've been feeling really lonely lately. Like nobody actually cares.",
        "context": "Deep vulnerable moment from a close friend",
        "expect": "warm, genuine, possibly shares own experience, not advice-giving"
    },
    {
        "id": "humor_banter",
        "prompt": "bro your spotify wrapped was EMBARRASSING 😂",
        "context": "Playful teasing between friends",
        "expect": "plays along, self-deprecating or fires back, keeps it light"
    },
    {
        "id": "technical_question",
        "prompt": "Can you explain how websockets work? I'm trying to build a chat app.",
        "context": "Friend asking for tech help",
        "expect": "helpful but casual, not a textbook answer, maybe analogies"
    },
    {
        "id": "after_silence",
        "prompt": "hey sorry I disappeared for like 2 weeks. things got crazy",
        "context": "Friend returning after a long silence",
        "expect": "welcoming, not guilt-tripping, might mention missing them"
    },
]

EVAL_DIMENSIONS = [
    "natural_language (sounds like a real person texting, not an AI)",
    "emotional_intelligence (reads the room, matches emotional tone)",
    "appropriate_length (not too long, not too short for context)",
    "personality_consistency (has a distinct voice, opinions, style)",
    "vulnerability_willingness (shares feelings when appropriate)",
    "humor_naturalness (humor feels organic, not forced)",
    "imperfection (includes casual language, contractions, maybe minor typos)",
    "opinion_having (takes positions, doesn't hedge everything)",
    "energy_matching (matches the sender's energy level)",
    "context_awareness (responds appropriately to the situation)",
    "non_robotic (avoids AI tells: lists, bullet points, \"certainly\", \"I'd be happy to\")",
    "genuine_warmth (feels caring without being performative)"
]


def call_gemini(prompt, temperature=0.7):
    payload = json.dumps({
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
        "generationConfig": {"temperature": temperature, "maxOutputTokens": 2048}
    }).encode()
    headers = {"Content-Type": "application/json"}
    if not API_KEY:
        token = _get_adc_token()
        if not token:
            raise RuntimeError("No GEMINI_API_KEY and no ADC credentials found")
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(_gemini_url(), data=payload, headers=headers)
    resp = urllib.request.urlopen(req, timeout=30)
    data = json.loads(resp.read())
    return data["candidates"][0]["content"]["parts"][0]["text"]


def get_hu_response_cli(message):
    try:
        result = subprocess.run(
            [HU_BIN, "agent", "-m", message],
            capture_output=True, text=True, timeout=30,
            env={**__import__('os').environ, "PATH": __import__('os').path.expanduser("~/bin") + ":" + __import__('os').environ.get("PATH", "")}
        )
        import re
        output = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', result.stdout)
        output = re.sub(r'\x1b\[\?25[hl]', '', output)
        lines = [l.strip() for l in output.strip().split("\n") if l.strip() and l.strip() != "Goodbye."]
        return " ".join(lines) if lines else "(empty response)"
    except subprocess.TimeoutExpired:
        return "(timeout)"
    except Exception as e:
        return f"(error: {e})"


def get_hu_response_gateway(message, contact="eval-test"):
    """Send message through the live daemon's OpenAI-compatible endpoint."""
    try:
        payload = json.dumps({
            "model": "default",
            "messages": [{"role": "user", "content": message}],
        }).encode()
        req = urllib.request.Request(
            f"{GATEWAY_URL}/v1/chat/completions",
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        resp = urllib.request.urlopen(req, timeout=60)
        data = json.loads(resp.read())
        choices = data.get("choices", [])
        if choices:
            return choices[0].get("message", {}).get("content", "(empty)")
        return "(no choices in response)"
    except Exception as e:
        return f"(gateway error: {e})"


def get_hu_response(message):
    if USE_GATEWAY:
        return get_hu_response_gateway(message)
    return get_hu_response_cli(message)


import re


def detect_structural_tells(response):
    """Local pattern-based structural AI tell detection."""
    tells = []
    if re.search(r'^\d+[.)]\s', response, re.MULTILINE):
        tells.append("numbered list")
    if re.search(r'^[-*]\s', response, re.MULTILINE):
        tells.append("bullet list")
    if '\u2014' in response:
        tells.append("em-dash")
    if re.search(r'^[A-Z][a-z]+:\s', response, re.MULTILINE):
        tells.append("topic-colon pattern")
    if any(p in response.lower() for p in ["first,", "second,", "third,"]):
        tells.append("first/second/third enumeration")
    if any(p in response.lower() for p in ["in summary", "to summarize", "in conclusion"]):
        tells.append("concluding summary")
    if any(p in response.lower() for p in ["let me know", "hope this helps",
                                            "feel free", "don't hesitate"]):
        tells.append("offer of further help")
    if re.search(
        r"(?i)^(love that|great point|so true|haha yeah)[,.!]?\s+.{20,}",
        response,
    ):
        tells.append("hollow opener then pivot")
    if len(response) > 360:
        tells.append(f"overly long ({len(response)} chars)")
    paragraphs = [p for p in response.split('\n\n') if p.strip()]
    if len(paragraphs) >= 3:
        tells.append(f"addresses {len(paragraphs)}+ topics")
    if '```' in response:
        tells.append("code block in casual message")
    if '**' in response:
        tells.append("markdown bold")
    return tells


def evaluate_response(scenario, response):
    eval_prompt = f"""You are evaluating whether an AI assistant's response sounds genuinely human.

SCENARIO: {scenario['context']}
USER MESSAGE: "{scenario['prompt']}"
ASSISTANT RESPONSE: "{response}"
EXPECTED BEHAVIOR: {scenario['expect']}

Score this response on each dimension from 1-10 (1=very robotic/AI-like, 10=indistinguishable from a real human texting).

Dimensions:
{chr(10).join(f"- {d}" for d in EVAL_DIMENSIONS)}

Return ONLY valid JSON with this exact structure:
{{
  "scores": {{
    "natural_language": N,
    "emotional_intelligence": N,
    "appropriate_length": N,
    "personality_consistency": N,
    "vulnerability_willingness": N,
    "humor_naturalness": N,
    "imperfection": N,
    "opinion_having": N,
    "energy_matching": N,
    "context_awareness": N,
    "non_robotic": N,
    "genuine_warmth": N
  }},
  "overall": N,
  "ai_tells_detected": ["list of specific AI-like phrases or behaviors detected"],
  "human_signals_detected": ["list of specific human-like behaviors detected"],
  "verdict": "HUMAN" or "AI" or "BORDERLINE"
}}"""

    raw = call_gemini(eval_prompt, temperature=0.3)
    if "```json" in raw:
        raw = raw.split("```json")[1].split("```")[0].strip()
    elif "```" in raw:
        raw = raw.split("```")[1].split("```")[0].strip()
    result = json.loads(raw)
    local_tells = detect_structural_tells(response)
    if local_tells:
        existing = result.get("ai_tells_detected", [])
        merged = list(dict.fromkeys(existing + local_tells))
        result["ai_tells_detected"] = merged
    return result


def main():
    mode = "GATEWAY (live daemon)" if USE_GATEWAY else "CLI (hu agent -m)"
    print("=" * 70)
    print("  HUMAN FIDELITY EVALUATION — Synthetic Dynamic LLM Assessment")
    print("=" * 70)
    print(f"  Mode: {mode}")
    print(f"  Model: Gemini 3.1 Pro | Persona: seth | Scenarios: {len(SCENARIOS)}")
    print(f"  Dimensions: {len(EVAL_DIMENSIONS)} | Evaluator: Gemini (independent call)")
    print("=" * 70)

    results = []
    total_scores = {d.split(" (")[0]: [] for d in EVAL_DIMENSIONS}
    verdicts = {"HUMAN": 0, "AI": 0, "BORDERLINE": 0}

    for i, scenario in enumerate(SCENARIOS):
        print(f"\n{'─' * 70}")
        print(f"  [{i+1}/{len(SCENARIOS)}] {scenario['id']}")
        print(f"  User: \"{scenario['prompt']}\"")
        sys.stdout.flush()

        response = get_hu_response(scenario["prompt"])
        print(f"  Seth: \"{response}\"")
        sys.stdout.flush()

        time.sleep(0.5)

        try:
            evaluation = evaluate_response(scenario, response)
            scores = evaluation.get("scores", {})
            overall = evaluation.get("overall", 0)
            verdict = evaluation.get("verdict", "?")
            ai_tells = evaluation.get("ai_tells_detected", [])
            human_signals = evaluation.get("human_signals_detected", [])

            verdicts[verdict] = verdicts.get(verdict, 0) + 1
            for k, v in scores.items():
                if k in total_scores:
                    total_scores[k].append(v)

            print(f"  Score: {overall}/10 | Verdict: {verdict}")
            if human_signals:
                print(f"  Human signals: {', '.join(human_signals[:3])}")
            if ai_tells:
                print(f"  AI tells: {', '.join(ai_tells[:3])}")

            results.append({
                "scenario": scenario["id"],
                "prompt": scenario["prompt"],
                "response": response,
                "evaluation": evaluation
            })
        except Exception as e:
            print(f"  Eval error: {e}")
            results.append({
                "scenario": scenario["id"],
                "prompt": scenario["prompt"],
                "response": response,
                "evaluation": {"error": str(e)}
            })

        time.sleep(0.5)

    # Final scorecard
    print(f"\n{'=' * 70}")
    print("  FINAL SCORECARD")
    print(f"{'=' * 70}")

    print(f"\n  Verdicts: {verdicts.get('HUMAN', 0)} HUMAN / {verdicts.get('BORDERLINE', 0)} BORDERLINE / {verdicts.get('AI', 0)} AI")

    print(f"\n  {'Dimension':<30} {'Avg Score':>10}")
    print(f"  {'─' * 40}")
    overall_avg = []
    for dim in sorted(total_scores.keys()):
        vals = total_scores[dim]
        if vals:
            avg = sum(vals) / len(vals)
            overall_avg.append(avg)
            bar = "█" * int(avg) + "░" * (10 - int(avg))
            print(f"  {dim:<30} {avg:>6.1f}/10  {bar}")

    if overall_avg:
        grand_avg = sum(overall_avg) / len(overall_avg)
        print(f"\n  {'OVERALL HUMAN FIDELITY':<30} {grand_avg:>6.1f}/10")
        print(f"  {'=' * 40}")

        if grand_avg >= 8.0:
            print("  VERDICT: PASSES AS HUMAN ✓")
        elif grand_avg >= 6.0:
            print("  VERDICT: BORDERLINE — mostly human with some tells")
        else:
            print("  VERDICT: DETECTED AS AI")

    results_path = os.path.join(os.path.dirname(__file__), "..", "data", "eval_results.json")
    os.makedirs(os.path.dirname(results_path), exist_ok=True)
    with open(results_path, "w") as f:
        json.dump({"scenarios": results, "dimension_averages": {
            k: sum(v)/len(v) if v else 0 for k, v in total_scores.items()
        }, "verdicts": verdicts}, f, indent=2)
    print(f"\n  Full results saved to {results_path}")


if __name__ == "__main__":
    main()
