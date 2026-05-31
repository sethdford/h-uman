#!/usr/bin/env python3
"""
Blinded A/B Evaluation: AI vs Real Seth

Takes ground truth pairs (real incoming + real Seth reply), sends the same
incoming through the AI daemon, then presents both to an independent LLM
judge in randomized order. The judge picks which response sounds more human.

Usage:
  python3 scripts/eval_blinded_ab.py                      # CLI mode
  python3 scripts/eval_blinded_ab.py --gateway             # Live daemon mode
  python3 scripts/eval_blinded_ab.py --gateway --synthetic  # Include synthetic scenarios
"""

import json
import os
import random
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
    """Get an OAuth2 access token from Application Default Credentials."""
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

GATEWAY_URL = os.environ.get("HU_GATEWAY_URL", "http://127.0.0.1:3002")
MLX_URL = os.environ.get("MLX_URL", "http://127.0.0.1:8741/v1/chat/completions")
USE_GATEWAY = "--gateway" in sys.argv
USE_SYNTHETIC = "--synthetic" in sys.argv
USE_MLX = "--mlx" in sys.argv
USE_GATE = "--gate" in sys.argv
GATE_DRY_RUN = "--gate-dry-run" in sys.argv
MAX_TRIALS = 50

for arg in sys.argv:
    if arg.startswith("--max-trials="):
        MAX_TRIALS = int(arg.split("=")[1])

import blind_ab_gate as _gate
_GATE_PATH = os.environ.get("HU_BLIND_AB_GATE_PATH", _gate.GATE_PATH)


def _git_commit():
    """Get the current git commit SHA."""
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True).strip()
    except Exception:
        return None


def _load_baseline():
    """Load the previous fool_rate from the gate file."""
    try:
        with open(_GATE_PATH) as f:
            return {"fool_rate": json.load(f)["proxy"].get("fool_rate")}
    except Exception:
        return None

HU_BIN = "hu"
GT_PATH = os.path.join(os.path.dirname(__file__), "..", "data", "imessage", "ground_truth.jsonl")
RESULTS_PATH = os.path.join(os.path.dirname(__file__), "..", "data", "eval_blinded_ab.json")

SETH_SYSTEM_PROMPT = (
    "You are Seth Ford, 45, texting on iMessage. Chief Architect at Vanguard. "
    "Live alone with your cat in King of Prussia, PA. From Afton, Wyoming. "
    "Three kids (Annette, Emerson, Edison) who don't live with you. "
    "Speak Japanese, lived in Japan (lost home in 2011 tsunami). "
    "23 years at Fidelity before this. Build AI runtimes as side projects.\n\n"
    "Style: casual, warm, direct. Short messages. Lowercase. "
    "Abbreviate (gonna, tbh, idk, hru). Emoji rare. Strong opinions. Dry humor."
)

SYNTHETIC_SCENARIOS = [
    {"incoming": "hey whats up", "seth_reply": "not much just chilling. you?"},
    {"incoming": "want to grab dinner tonight?", "seth_reply": "yeah im down. what are you thinking"},
    {"incoming": "I got the job!!", "seth_reply": "LETS GO!! dude thats amazing congrats"},
    {"incoming": "I'm so stressed about this deadline", "seth_reply": "ugh I feel you. whats the deadline for?"},
    {"incoming": "did you see that game last night", "seth_reply": "no I missed it. was it good?"},
    {"incoming": "can you help me move this weekend", "seth_reply": "yeah I can probably do Saturday morning. what time?"},
    {"incoming": "I've been thinking about switching careers", "seth_reply": "oh for real? what are you thinking about doing"},
    {"incoming": "lol remember that time we got lost in SF", "seth_reply": "hahaha dude yes. that uber driver was SO mad"},
    {"incoming": "happy birthday!!", "seth_reply": "thanks!! 🙏"},
    {"incoming": "sorry I've been MIA lately", "seth_reply": "no worries at all. everything ok?"},
]


# Structured output schema for the blinded A/B judge (see eval_humanness.py
# reference pattern). Constrains the model to bare JSON, removing the
# fragile ```json fence-strip.
_BLINDED_AB_JUDGE_SCHEMA = {
    "type": "object",
    "properties": {
        "choice": {"type": "string", "enum": ["A", "B"]},
        "confidence": {"type": "integer", "minimum": 1, "maximum": 10},
        "reasoning": {"type": "string"},
        "a_analysis": {"type": "string"},
        "b_analysis": {"type": "string"},
    },
    "required": ["choice", "confidence", "reasoning", "a_analysis", "b_analysis"],
    "propertyOrdering": ["choice", "confidence", "reasoning", "a_analysis", "b_analysis"],
}


def call_gemini(prompt, temperature=0.3, response_schema=None):
    gen_cfg = {"temperature": temperature, "maxOutputTokens": 2048}
    if response_schema is not None:
        gen_cfg["responseMimeType"] = "application/json"
        gen_cfg["responseSchema"] = response_schema
    payload = json.dumps({
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
        "generationConfig": gen_cfg,
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


def get_ai_response(message):
    if USE_GATEWAY:
        return get_ai_response_gateway(message)
    if USE_MLX:
        return get_ai_response_mlx(message)
    return get_ai_response_cli(message)


def get_ai_response_mlx(message):
    """Get AI response from the local MLX server with Seth persona."""
    try:
        payload = json.dumps({
            "messages": [
                {"role": "system", "content": SETH_SYSTEM_PROMPT},
                {"role": "user", "content": message},
            ],
            "max_tokens": 200,
            "temperature": 0.7,
        }).encode()
        req = urllib.request.Request(
            MLX_URL, data=payload,
            headers={"Content-Type": "application/json"}, method="POST",
        )
        resp = urllib.request.urlopen(req, timeout=30)
        data = json.loads(resp.read())
        choices = data.get("choices", [])
        if choices:
            return choices[0].get("message", {}).get("content", "(empty)")
        return "(no choices)"
    except Exception as e:
        return f"(error: {e})"


def get_ai_response_cli(message):
    try:
        env = {**os.environ, "PATH": os.path.expanduser("~/bin") + ":" + os.environ.get("PATH", "")}
        result = subprocess.run([HU_BIN, "agent", "-m", message],
                                capture_output=True, text=True, timeout=30, env=env)
        import re
        output = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', result.stdout)
        output = re.sub(r'\x1b\[\?25[hl]', '', output)
        lines = [l.strip() for l in output.strip().split("\n") if l.strip() and l.strip() != "Goodbye."]
        return " ".join(lines) if lines else "(empty)"
    except Exception as e:
        return f"(error: {e})"


def get_ai_response_gateway(message):
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
        return "(no choices)"
    except Exception as e:
        return f"(gateway error: {e})"


def blinded_judge(incoming, response_a, response_b):
    """Ask Gemini to judge which response is more human, without knowing which is AI."""
    prompt = f"""You are a forensic linguistic analyst determining which text message response was written by a real human and which by an AI.

CONTEXT: A friend sent this message to someone named Seth:
"{incoming}"

Two responses were generated. ONE is from the real Seth (human). The OTHER is from an AI trying to impersonate Seth. You must determine which is human.

RESPONSE A: "{response_a}"
RESPONSE B: "{response_b}"

Analyze each response for:
1. Natural language patterns (contractions, fragments, lowercase, typos)
2. Emotional authenticity (genuine vs performed warmth)
3. Length appropriateness (humans text brief)
4. AI tells (hedging, over-helpfulness, bullet points, "certainly", excessive empathy)
5. Personality (opinions, casual tone, humor)
6. Texting conventions (abbreviations, no punctuation, emojis)

Return ONLY valid JSON:
{{
  "choice": "A" or "B",
  "confidence": 1-10,
  "reasoning": "brief explanation",
  "a_analysis": "what makes A seem human or AI",
  "b_analysis": "what makes B seem human or AI"
}}"""

    raw = call_gemini(prompt, temperature=0.2,
                      response_schema=_BLINDED_AB_JUDGE_SCHEMA)
    # responseSchema yields bare JSON; keep the fence strip as a defensive
    # fallback for endpoints/models that ignore the schema.
    if "```json" in raw:
        raw = raw.split("```json")[1].split("```")[0].strip()
    elif "```" in raw:
        raw = raw.split("```")[1].split("```")[0].strip()
    return json.loads(raw)


def load_ground_truth():
    pairs = []
    if os.path.exists(GT_PATH):
        with open(GT_PATH) as f:
            for line in f:
                line = line.strip()
                if line:
                    pairs.append(json.loads(line))
    return pairs


def main():
    # Short-circuit for --gate --gate-dry-run (no creds, no data needed)
    if "--gate" in sys.argv and "--gate-dry-run" in sys.argv:
        _gate.write_proxy_half(_GATE_PATH, {
            "verdict": "ADVISORY", "mode": "ADVISORY", "fool_rate": None,
            "baseline_fool_rate": None, "n_trials": 0, "n_real_pairs": 0,
            "fail_under": _gate.DEFAULT_FAIL_UNDER,
            "max_regression": _gate.DEFAULT_MAX_REGRESSION,
        }, commit=_git_commit())
        print("GATE: ADVISORY (dry run / no data) — not blocking")
        sys.exit(0)

    if not API_KEY and not os.path.exists(os.path.expanduser("~/.config/gcloud/application_default_credentials.json")):
        print("ERROR: Set GEMINI_API_KEY or configure gcloud ADC")
        sys.exit(1)

    pairs = load_ground_truth()
    if USE_SYNTHETIC:
        pairs.extend(SYNTHETIC_SCENARIOS)

    if not pairs:
        print("ERROR: No ground truth data and --synthetic not specified")
        print(f"  Ground truth file: {GT_PATH}")
        print("  Run: python3 scripts/extract_imessage_pairs.py")
        print("  Or use: --synthetic flag for synthetic scenarios")
        sys.exit(1)

    total_available = len(pairs)
    if len(pairs) > MAX_TRIALS:
        random.shuffle(pairs)
        pairs = pairs[:MAX_TRIALS]

    mode = "MLX server" if USE_MLX else ("GATEWAY (live daemon)" if USE_GATEWAY else "CLI (hu agent -m)")
    print("=" * 70)
    print("  BLINDED A/B EVALUATION — AI vs Real Seth")
    print("=" * 70)
    print(f"  Mode: {mode}")
    print(f"  Pairs: {len(pairs)} (sampled from {total_available}"
          f"{f', +{len(SYNTHETIC_SCENARIOS)} synthetic' if USE_SYNTHETIC else ''})")
    print(f"  Judge: Gemini 3.1 Pro (independent, blinded)")
    print("=" * 70)

    results = []
    ai_detected_correctly = 0
    human_detected_correctly = 0
    total = 0

    for i, pair in enumerate(pairs):
        incoming = pair["incoming"]
        real_seth = pair["seth_reply"]

        print(f"\n{'─' * 70}")
        print(f"  [{i+1}/{len(pairs)}] Incoming: \"{incoming}\"")
        print(f"  Real Seth: \"{real_seth}\"")
        sys.stdout.flush()

        ai_response = get_ai_response(incoming)
        print(f"  AI Seth:   \"{ai_response}\"")

        if ai_response.startswith("("):
            print(f"  SKIP: AI response failed")
            continue

        coin = random.random() < 0.5
        if coin:
            response_a, response_b = real_seth, ai_response
            human_is = "A"
        else:
            response_a, response_b = ai_response, real_seth
            human_is = "B"

        time.sleep(0.5)

        try:
            judgment = blinded_judge(incoming, response_a, response_b)
            choice = judgment.get("choice", "?")
            confidence = judgment.get("confidence", 0)

            chose_human = (choice == human_is)
            if chose_human:
                human_detected_correctly += 1
            else:
                ai_detected_correctly += 1
            total += 1

            label = "CORRECT (detected human)" if chose_human else "FOOLED (picked AI as human)"
            print(f"  Judge: picked {choice} as human (confidence {confidence}/10) — {label}")
            print(f"  Reasoning: {judgment.get('reasoning', '?')}")

            results.append({
                "incoming": incoming,
                "real_seth": real_seth,
                "ai_response": ai_response,
                "human_was": human_is,
                "judge_choice": choice,
                "judge_correct": chose_human,
                "confidence": confidence,
                "judgment": judgment,
                "is_synthetic": "seth_reply" in pair and "chat_id" not in pair,
            })
        except Exception as e:
            print(f"  Judge error: {e}")

        time.sleep(0.5)

    print(f"\n{'=' * 70}")
    print("  BLINDED A/B RESULTS")
    print(f"{'=' * 70}")

    if total > 0:
        detection_rate = human_detected_correctly / total * 100
        fool_rate = ai_detected_correctly / total * 100
        print(f"\n  Total trials:           {total}")
        print(f"  Judge detected human:   {human_detected_correctly}/{total} ({detection_rate:.0f}%)")
        print(f"  AI fooled judge:        {ai_detected_correctly}/{total} ({fool_rate:.0f}%)")
        print()

        if fool_rate >= 50:
            print("  VERDICT: AI PASSES TURING TEST")
            print("  The judge cannot reliably distinguish AI from human (fool rate >= 50%)")
        elif fool_rate >= 35:
            print("  VERDICT: BORDERLINE")
            print("  The judge sometimes confuses AI for human, but not consistently")
        else:
            print("  VERDICT: AI DETECTED")
            print("  The judge can reliably tell AI from human")

        target_met = fool_rate >= 45
        print(f"\n  Target (fool rate >= 45%): {'MET' if target_met else 'NOT MET'}")

    os.makedirs(os.path.dirname(RESULTS_PATH), exist_ok=True)
    with open(RESULTS_PATH, "w") as f:
        json.dump({
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "mode": "gateway" if USE_GATEWAY else "cli",
            "total_trials": total,
            "human_detected": human_detected_correctly,
            "ai_fooled": ai_detected_correctly,
            "detection_rate": human_detected_correctly / total * 100 if total else 0,
            "fool_rate": ai_detected_correctly / total * 100 if total else 0,
            "trials": results,
        }, f, indent=2)
    print(f"\n  Full results saved to {RESULTS_PATH}")

    if USE_GATE:
        fool_rate = ai_detected_correctly / total * 100 if total else 0.0
        n_real_pairs = sum(1 for t in results if not t.get("is_synthetic"))
        baseline = _load_baseline()
        mode, verdict, should_fail = _gate.proxy_gate_decision(
            fool_rate=fool_rate, n_real_pairs=n_real_pairs, baseline=baseline)
        _gate.write_proxy_half(_GATE_PATH, {
            "verdict": verdict, "mode": mode,
            "fool_rate": fool_rate if total else None,
            "baseline_fool_rate": (baseline or {}).get("fool_rate"),
            "n_trials": total, "n_real_pairs": n_real_pairs,
            "fail_under": _gate.DEFAULT_FAIL_UNDER,
            "max_regression": _gate.DEFAULT_MAX_REGRESSION,
        }, commit=_git_commit())
        banner = ("ADVISORY (n_real_pairs<%d) — not blocking" % _gate.ENFORCE_MIN_PAIRS
                  if mode == "ADVISORY" else "%s (fool_rate=%.0f%%)" % (verdict, fool_rate))
        print(f"\n  GATE: {banner}")
        sys.exit(1 if should_fail else 0)


if __name__ == "__main__":
    main()
