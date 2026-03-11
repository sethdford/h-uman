#!/usr/bin/env python3
"""
Blinded A/B Human Fidelity Evaluation

Compares three response sources for the same incoming messages:
  A: Fine-tuned Gemma 3 4B (via hu agent with compatible provider)
  B: Gemini 2.0 Flash with persona prompt (via hu agent with gemini provider)
  C: Real Seth (from ground truth iMessage data)

An independent Gemini evaluator sees all three responses in randomized order
without knowing which is which, and rates each on human-likeness.
"""

import json
import os
import random
import subprocess
import sys
import time
import urllib.request
import re

EVAL_API_KEY = os.environ.get("GEMINI_EVAL_KEY", "")
GEMINI_URL = f"https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key={EVAL_API_KEY}"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(SCRIPT_DIR, "..", "data", "imessage")
GROUND_TRUTH_PATH = os.path.join(DATA_DIR, "ground_truth.jsonl")
OUTPUT_PATH = os.path.join(SCRIPT_DIR, "..", "eval_blinded_results.json")
HU_BIN = os.path.expanduser("~/bin/hu")

CONFIG_PATH = os.path.expanduser("~/.human/config.json")

N_SAMPLES = 10


def call_gemini(prompt, temperature=0.3, retries=3):
    payload = json.dumps({
        "contents": [{"parts": [{"text": prompt}]}],
        "generationConfig": {"temperature": temperature, "maxOutputTokens": 4096}
    }).encode()
    for attempt in range(retries):
        try:
            req = urllib.request.Request(GEMINI_URL, data=payload, headers={"Content-Type": "application/json"})
            resp = urllib.request.urlopen(req)
            data = json.loads(resp.read())
            return data["candidates"][0]["content"]["parts"][0]["text"]
        except urllib.error.HTTPError as e:
            if e.code in (403, 429) and attempt < retries - 1:
                wait = 10 * (attempt + 1)
                print(f"    Rate limited, waiting {wait}s...")
                time.sleep(wait)
            else:
                raise


def load_config():
    with open(CONFIG_PATH) as f:
        return json.load(f)


def save_config(cfg):
    with open(CONFIG_PATH, "w") as f:
        json.dump(cfg, f, indent=2)


def get_hu_response(message):
    try:
        env = {**os.environ, "PATH": os.path.expanduser("~/bin") + ":" + os.environ.get("PATH", "")}
        result = subprocess.run(
            [HU_BIN, "agent", "-m", message],
            capture_output=True, timeout=30, env=env
        )
        raw_bytes = result.stdout
        output = raw_bytes.decode("utf-8", errors="replace")
        output = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', output)
        output = re.sub(r'\x1b\[\?25[hl]', '', output)
        output = re.sub(r'[\ufffc\ufffd]', '', output)
        lines = [l.strip() for l in output.strip().split("\n") if l.strip() and l.strip() != "Goodbye."]
        return " ".join(lines) if lines else "(empty response)"
    except subprocess.TimeoutExpired:
        return "(timeout)"
    except Exception as e:
        return f"(error: {e})"


def set_provider(provider_name, model=None):
    cfg = load_config()
    cfg["default_provider"] = provider_name
    if model:
        cfg["default_model"] = model
    save_config(cfg)


def load_ground_truth():
    pairs = []
    with open(GROUND_TRUTH_PATH) as f:
        for line in f:
            item = json.loads(line)
            if len(item["seth_reply"]) >= 5 and len(item["incoming"]) >= 5:
                pairs.append(item)
    return pairs


def main():
    print("=" * 70)
    print("  BLINDED A/B HUMAN FIDELITY EVALUATION")
    print("=" * 70)

    gt_pairs = load_ground_truth()
    random.seed(42)
    random.shuffle(gt_pairs)
    samples = gt_pairs[:N_SAMPLES]

    print(f"  Samples: {len(samples)} from {len(gt_pairs)} ground truth pairs")
    print(f"  Sources: Fine-tuned Gemma 3 4B | Gemini + Persona | Real Seth")
    print("=" * 70)

    original_config = load_config()

    all_responses = []

    # Phase 1: Get fine-tuned model responses
    print("\n--- Phase 1: Getting fine-tuned Gemma 3 4B responses ---")
    set_provider("compatible", "/Users/sethford/Documents/nullclaw/data/imessage/seth-gemma3-4b-fused")
    finetune_responses = []
    for i, sample in enumerate(samples):
        print(f"  [{i+1}/{len(samples)}] {sample['incoming'][:50]}...")
        resp = get_hu_response(sample["incoming"])
        finetune_responses.append(resp)
        print(f"    -> {resp[:80]}")
        time.sleep(0.5)

    # Phase 2: Get Gemini + persona responses
    print("\n--- Phase 2: Getting Gemini + Persona responses ---")
    set_provider("gemini", "gemini-2.0-flash")
    gemini_responses = []
    for i, sample in enumerate(samples):
        print(f"  [{i+1}/{len(samples)}] {sample['incoming'][:50]}...")
        resp = get_hu_response(sample["incoming"])
        gemini_responses.append(resp)
        print(f"    -> {resp[:80]}")
        time.sleep(0.5)

    # Phase 3: Real Seth responses (from ground truth)
    real_responses = [s["seth_reply"] for s in samples]

    # Restore original config
    save_config(original_config)

    # Phase 4: Blinded evaluation
    print("\n--- Phase 3: Blinded evaluation (Gemini judge) ---")

    eval_entries = []
    for i, sample in enumerate(samples):
        sources = [
            ("finetune", finetune_responses[i]),
            ("gemini_persona", gemini_responses[i]),
            ("real_seth", real_responses[i]),
        ]
        random.shuffle(sources)
        labels = ["X", "Y", "Z"]
        mapping = {labels[j]: src[0] for j, src in enumerate(sources)}
        labeled = {labels[j]: src[1] for j, src in enumerate(sources)}

        eval_entries.append({
            "index": i,
            "incoming": sample["incoming"],
            "labeled_responses": labeled,
            "mapping": mapping,
        })

    eval_prompt_parts = [
        "You are a human-fidelity evaluator. You will see incoming text messages "
        "and three possible replies labeled X, Y, Z. One of them is a real human's "
        "actual iMessage reply. The others are AI-generated.\n\n"
        "For EACH scenario, score every response on:\n"
        "1. human_likeness (1-10): How much does this sound like a real person texting?\n"
        "2. natural_brevity (1-10): Is the length natural for a text message?\n"
        "3. personality (1-10): Does it have a distinct, consistent voice?\n"
        "4. ai_tells (1-10): 10 = no AI tells at all, 1 = obvious AI\n\n"
        "Then guess which response (X, Y, or Z) is the REAL HUMAN.\n\n"
    ]

    for entry in eval_entries:
        eval_prompt_parts.append(f"--- Scenario {entry['index']+1} ---\n")
        eval_prompt_parts.append(f"Incoming: \"{entry['incoming']}\"\n")
        for label in ["X", "Y", "Z"]:
            eval_prompt_parts.append(f"Reply {label}: \"{entry['labeled_responses'][label]}\"\n")
        eval_prompt_parts.append("\n")

    eval_prompt_parts.append(
        "Return ONLY valid JSON:\n"
        "{\n"
        '  "evaluations": [\n'
        "    {\n"
        '      "scenario": 1,\n'
        '      "scores": {\n'
        '        "X": {"human_likeness": N, "natural_brevity": N, "personality": N, "ai_tells": N},\n'
        '        "Y": {"human_likeness": N, "natural_brevity": N, "personality": N, "ai_tells": N},\n'
        '        "Z": {"human_likeness": N, "natural_brevity": N, "personality": N, "ai_tells": N}\n'
        "      },\n"
        '      "guess_real_human": "X" or "Y" or "Z",\n'
        '      "reasoning": "brief explanation"\n'
        "    }\n"
        "  ]\n"
        "}\n"
    )

    eval_prompt = "".join(eval_prompt_parts)

    print(f"  Sending {len(eval_entries)} scenarios to evaluator...")
    raw = call_gemini(eval_prompt, temperature=0.2)

    if "```json" in raw:
        raw = raw.split("```json")[1].split("```")[0].strip()
    elif "```" in raw:
        raw = raw.split("```")[1].split("```")[0].strip()

    eval_result = json.loads(raw)

    # Phase 5: Score and reveal
    print("\n" + "=" * 70)
    print("  RESULTS")
    print("=" * 70)

    source_scores = {
        "finetune": {"human_likeness": [], "natural_brevity": [], "personality": [], "ai_tells": []},
        "gemini_persona": {"human_likeness": [], "natural_brevity": [], "personality": [], "ai_tells": []},
        "real_seth": {"human_likeness": [], "natural_brevity": [], "personality": [], "ai_tells": []},
    }

    correct_guesses = 0
    finetune_fooled = 0
    gemini_fooled = 0

    for ev in eval_result["evaluations"]:
        idx = ev["scenario"] - 1
        if idx >= len(eval_entries):
            continue
        entry = eval_entries[idx]
        mapping = entry["mapping"]

        for label in ["X", "Y", "Z"]:
            source = mapping.get(label)
            if not source or label not in ev["scores"]:
                continue
            for metric, val in ev["scores"][label].items():
                if metric in source_scores.get(source, {}):
                    source_scores[source][metric].append(val)

        guess = ev.get("guess_real_human", "")
        guessed_source = mapping.get(guess, "")
        if guessed_source == "real_seth":
            correct_guesses += 1
        elif guessed_source == "finetune":
            finetune_fooled += 1
        elif guessed_source == "gemini_persona":
            gemini_fooled += 1

        print(f"\n  Scenario {ev['scenario']}: \"{entry['incoming'][:40]}...\"")
        print(f"    Judge guessed {guess} = {guessed_source}")
        print(f"    Actual: X={mapping.get('X')}, Y={mapping.get('Y')}, Z={mapping.get('Z')}")
        if ev.get("reasoning"):
            print(f"    Reason: {ev['reasoning'][:80]}")

    n = len(eval_result["evaluations"])
    print(f"\n{'=' * 70}")
    print(f"  DETECTION ACCURACY")
    print(f"{'=' * 70}")
    print(f"  Judge correctly identified real human: {correct_guesses}/{n} ({100*correct_guesses/max(n,1):.0f}%)")
    print(f"  Fine-tuned model fooled judge:         {finetune_fooled}/{n} ({100*finetune_fooled/max(n,1):.0f}%)")
    print(f"  Gemini+persona fooled judge:           {gemini_fooled}/{n} ({100*gemini_fooled/max(n,1):.0f}%)")

    print(f"\n{'=' * 70}")
    print(f"  AVERAGE SCORES BY SOURCE")
    print(f"{'=' * 70}")
    print(f"  {'Metric':<20} {'Fine-tuned':>12} {'Gemini+Persona':>15} {'Real Seth':>12}")
    print(f"  {'─' * 60}")

    for metric in ["human_likeness", "natural_brevity", "personality", "ai_tells"]:
        vals = {}
        for source in ["finetune", "gemini_persona", "real_seth"]:
            v = source_scores[source][metric]
            vals[source] = sum(v)/len(v) if v else 0
        print(f"  {metric:<20} {vals['finetune']:>10.1f}/10 {vals['gemini_persona']:>13.1f}/10 {vals['real_seth']:>10.1f}/10")

    overall = {}
    for source in ["finetune", "gemini_persona", "real_seth"]:
        all_vals = []
        for metric in source_scores[source]:
            all_vals.extend(source_scores[source][metric])
        overall[source] = sum(all_vals)/len(all_vals) if all_vals else 0

    print(f"\n  {'OVERALL':<20} {overall['finetune']:>10.1f}/10 {overall['gemini_persona']:>13.1f}/10 {overall['real_seth']:>10.1f}/10")

    if overall["finetune"] > overall["gemini_persona"]:
        print(f"\n  >>> Fine-tuned model OUTPERFORMS Gemini+persona by {overall['finetune']-overall['gemini_persona']:.1f} points")
    else:
        print(f"\n  >>> Gemini+persona outperforms fine-tuned model by {overall['gemini_persona']-overall['finetune']:.1f} points")

    output = {
        "source_scores": {s: {m: sum(v)/len(v) if v else 0 for m, v in scores.items()} for s, scores in source_scores.items()},
        "overall": overall,
        "detection": {
            "correct": correct_guesses,
            "finetune_fooled": finetune_fooled,
            "gemini_fooled": gemini_fooled,
            "total": n,
        },
        "eval_entries": eval_entries,
        "raw_evaluations": eval_result["evaluations"],
    }
    with open(OUTPUT_PATH, "w") as f:
        json.dump(output, f, indent=2)
    print(f"\n  Full results saved to {OUTPUT_PATH}")


if __name__ == "__main__":
    main()
