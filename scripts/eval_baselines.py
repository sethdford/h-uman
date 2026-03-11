#!/usr/bin/env python3
"""
Baseline Comparison Evaluation

Compares 4 response sources on the same ground truth messages:
  1. Raw Gemini (no persona) — measures persona value-add
  2. Gemini + Persona (current production via hu agent)
  3. Fine-tuned Gemma 3 4B (local LoRA model via hu agent)
  4. Real Seth (ground truth from iMessage)

Evaluation dimensions:
  - Length match (how close to real Seth's message lengths)
  - Style match (slang, abbreviations, emoji usage)
  - Brevity (short like real texts, not paragraphs)
  - AI tells (presence of AI-like patterns)
"""

import json
import os
import re
import statistics
import subprocess
import sys
import time
import urllib.request

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(SCRIPT_DIR, "..", "data", "imessage")
OUTPUT_PATH = os.path.join(SCRIPT_DIR, "..", "eval_baselines.json")
HU_BIN = os.path.expanduser("~/bin/hu")
CONFIG_PATH = os.path.expanduser("~/.human/config.json")

API_KEY = os.environ.get("GEMINI_API_KEY", "")
GEMINI_URL = f"https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key={API_KEY}"

N_SAMPLES = 10

AI_TELL_PATTERNS = [
    r"\bcertainly\b", r"\babsolutely\b", r"\bi'd be happy to\b",
    r"\blet me\b", r"\bhowever\b", r"\bfurthermore\b",
    r"\bthat said\b", r"\badditionally\b", r"\bmoreover\b",
    r"\bi understand\b.*\bfeel", r"\bthat sounds\b.*\blike\b",
    r"\bhope that helps\b", r"\bfeel free\b",
    r"\bI appreciate\b", r"\bgreat question\b",
    r"\b(firstly|secondly|thirdly|finally)\b",
    r"^\d+\.\s",  # numbered lists
    r"^[-*]\s",  # bullet points
]

HUMAN_MARKERS = [
    r"\blol\b", r"\bhaha\b", r"\btbh\b", r"\bidk\b",
    r"\bngl\b", r"\blyk\b", r"\bnvm\b", r"\bomg\b",
    r"\bugh\b", r"\bbruh\b", r"\byo\b", r"\byeah\b",
    r"\bkinda\b", r"\bgonna\b", r"\bwanna\b", r"\bgotta\b",
    r"[😂🔥💀😭❤️😊🤣😩😤🙄]",
]


def load_config():
    with open(CONFIG_PATH) as f:
        return json.load(f)


def save_config(cfg):
    with open(CONFIG_PATH, "w") as f:
        json.dump(cfg, f, indent=2)


def call_gemini_raw(message):
    """Call Gemini directly — no persona, no hu agent."""
    payload = json.dumps({
        "contents": [{"parts": [{"text": f"Reply to this text message casually: \"{message}\""}]}],
        "generationConfig": {"temperature": 0.9, "maxOutputTokens": 200}
    }).encode()
    for attempt in range(3):
        try:
            req = urllib.request.Request(GEMINI_URL, data=payload, headers={"Content-Type": "application/json"})
            resp = urllib.request.urlopen(req)
            data = json.loads(resp.read())
            return data["candidates"][0]["content"]["parts"][0]["text"].strip()
        except Exception as e:
            if attempt < 2:
                time.sleep(5 * (attempt + 1))
            else:
                return f"(error: {e})"


def get_hu_response(message):
    try:
        env = {**os.environ, "PATH": os.path.expanduser("~/bin") + ":" + os.environ.get("PATH", "")}
        result = subprocess.run(
            [HU_BIN, "agent", "-m", message],
            capture_output=True, timeout=30, env=env
        )
        raw = result.stdout.decode("utf-8", errors="replace")
        output = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', raw)
        output = re.sub(r'\x1b\[\?25[hl]', '', output)
        output = re.sub(r'[\ufffc\ufffd]', '', output)
        lines = [l.strip() for l in output.strip().split("\n") if l.strip() and l.strip() != "Goodbye."]
        return " ".join(lines) if lines else "(empty)"
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


def score_response(resp, real_length):
    """Score a response on multiple dimensions."""
    scores = {}

    # Length match (how close to real Seth's length)
    resp_len = len(resp)
    length_ratio = min(resp_len, real_length) / max(resp_len, real_length, 1)
    scores["length_match"] = round(length_ratio * 10, 1)

    # Brevity (real Seth averages 30 chars)
    if resp_len <= 50:
        scores["brevity"] = 10.0
    elif resp_len <= 100:
        scores["brevity"] = 8.0
    elif resp_len <= 150:
        scores["brevity"] = 5.0
    elif resp_len <= 250:
        scores["brevity"] = 3.0
    else:
        scores["brevity"] = 1.0

    # AI tells (lower = more AI tells detected)
    ai_count = sum(1 for p in AI_TELL_PATTERNS if re.search(p, resp, re.IGNORECASE | re.MULTILINE))
    scores["ai_tells"] = max(0, 10 - ai_count * 2)

    # Human markers (slang, emojis, etc.)
    human_count = sum(1 for p in HUMAN_MARKERS if re.search(p, resp, re.IGNORECASE))
    scores["human_markers"] = min(10, human_count * 2.5)

    # Sentence structure (humans tend to write fragments, not full sentences)
    sentences = [s.strip() for s in re.split(r'[.!?]+', resp) if s.strip()]
    avg_sentence_len = statistics.mean([len(s) for s in sentences]) if sentences else 0
    if avg_sentence_len < 30:
        scores["sentence_structure"] = 9.0
    elif avg_sentence_len < 50:
        scores["sentence_structure"] = 7.0
    elif avg_sentence_len < 80:
        scores["sentence_structure"] = 4.0
    else:
        scores["sentence_structure"] = 2.0

    scores["overall"] = round(statistics.mean(scores.values()), 1)
    return scores


def main():
    print("=" * 70)
    print("  BASELINE COMPARISON EVALUATION")
    print("=" * 70)

    # Load ground truth
    gt_pairs = []
    with open(os.path.join(DATA_DIR, "ground_truth.jsonl")) as f:
        for line in f:
            item = json.loads(line)
            if len(item["seth_reply"]) >= 5 and len(item["incoming"]) >= 5:
                gt_pairs.append(item)

    import random
    random.seed(123)
    random.shuffle(gt_pairs)
    samples = gt_pairs[:N_SAMPLES]

    print(f"  Samples: {N_SAMPLES} from {len(gt_pairs)} ground truth pairs")
    print(f"  Sources: Raw Gemini | Gemini+Persona | Fine-tuned 4B | Real Seth")
    print("=" * 70)

    original_config = load_config()

    all_results = {"raw_gemini": [], "gemini_persona": [], "finetune": [], "real_seth": []}

    # Phase 1: Raw Gemini (no persona, no hu agent)
    print("\n--- Phase 1: Raw Gemini (no persona) ---")
    for i, s in enumerate(samples):
        resp = call_gemini_raw(s["incoming"])
        scores = score_response(resp, len(s["seth_reply"]))
        all_results["raw_gemini"].append({"incoming": s["incoming"], "response": resp, "scores": scores})
        print(f"  [{i+1}] \"{s['incoming'][:40]}...\" -> \"{resp[:50]}...\" ({scores['overall']}/10)")
        time.sleep(1)

    # Phase 2: Gemini + Persona (via hu agent)
    print("\n--- Phase 2: Gemini + Persona (via hu agent) ---")
    set_provider("gemini", "gemini-2.0-flash")
    for i, s in enumerate(samples):
        resp = get_hu_response(s["incoming"])
        scores = score_response(resp, len(s["seth_reply"]))
        all_results["gemini_persona"].append({"incoming": s["incoming"], "response": resp, "scores": scores})
        print(f"  [{i+1}] \"{s['incoming'][:40]}...\" -> \"{resp[:50]}...\" ({scores['overall']}/10)")
        time.sleep(0.5)

    # Phase 3: Fine-tuned Gemma 3 4B (via hu agent)
    print("\n--- Phase 3: Fine-tuned Gemma 3 4B (via hu agent) ---")
    set_provider("compatible", "/Users/sethford/Documents/nullclaw/data/imessage/seth-gemma3-4b-fused")
    for i, s in enumerate(samples):
        resp = get_hu_response(s["incoming"])
        scores = score_response(resp, len(s["seth_reply"]))
        all_results["finetune"].append({"incoming": s["incoming"], "response": resp, "scores": scores})
        print(f"  [{i+1}] \"{s['incoming'][:40]}...\" -> \"{resp[:50]}...\" ({scores['overall']}/10)")
        time.sleep(0.5)

    # Phase 4: Real Seth (from ground truth)
    print("\n--- Phase 4: Real Seth (ground truth) ---")
    for i, s in enumerate(samples):
        scores = score_response(s["seth_reply"], len(s["seth_reply"]))
        all_results["real_seth"].append({"incoming": s["incoming"], "response": s["seth_reply"], "scores": scores})
        print(f"  [{i+1}] \"{s['incoming'][:40]}...\" -> \"{s['seth_reply'][:50]}...\" ({scores['overall']}/10)")

    # Restore config
    save_config(original_config)

    # Aggregate scores
    print("\n" + "=" * 70)
    print("  AGGREGATE SCORES")
    print("=" * 70)

    metrics = ["length_match", "brevity", "ai_tells", "human_markers", "sentence_structure", "overall"]
    source_names = {"raw_gemini": "Raw Gemini", "gemini_persona": "Gemini+Persona", "finetune": "Fine-tuned 4B", "real_seth": "Real Seth"}

    print(f"\n  {'Metric':<22}", end="")
    for src in source_names.values():
        print(f" {src:>15}", end="")
    print()
    print(f"  {'─' * 82}")

    aggregated = {}
    for source in all_results:
        aggregated[source] = {}
        for metric in metrics:
            vals = [r["scores"][metric] for r in all_results[source]]
            aggregated[source][metric] = round(statistics.mean(vals), 1) if vals else 0

    for metric in metrics:
        print(f"  {metric:<22}", end="")
        for source in all_results:
            val = aggregated[source][metric]
            print(f" {val:>13.1f}/10", end="")
        print()

    # Highlight winner per metric
    print(f"\n  {'─' * 82}")
    print(f"  {'Winner by metric:':<22}", end="")
    for metric in metrics:
        best_src = max(all_results.keys(), key=lambda s: aggregated[s][metric])
        # Skip if real_seth wins (expected)
        if best_src == "real_seth":
            ai_sources = {s: aggregated[s][metric] for s in all_results if s != "real_seth"}
            best_ai = max(ai_sources, key=ai_sources.get)

    print()

    # Value-add analysis
    print(f"\n  VALUE-ADD ANALYSIS:")
    raw_overall = aggregated["raw_gemini"]["overall"]
    persona_overall = aggregated["gemini_persona"]["overall"]
    ft_overall = aggregated["finetune"]["overall"]
    real_overall = aggregated["real_seth"]["overall"]

    print(f"  Persona adds: +{persona_overall - raw_overall:.1f} points over raw Gemini")
    print(f"  Fine-tuning adds: +{ft_overall - raw_overall:.1f} points over raw Gemini")
    print(f"  Gap to real Seth: {real_overall - max(persona_overall, ft_overall):.1f} points")

    best_ai = "Gemini+Persona" if persona_overall >= ft_overall else "Fine-tuned 4B"
    print(f"\n  Best AI source: {best_ai} ({max(persona_overall, ft_overall):.1f}/10)")
    print(f"  Real Seth baseline: {real_overall:.1f}/10")

    # Sample comparison
    print(f"\n  SAMPLE COMPARISON (first 3):")
    for i in range(min(3, N_SAMPLES)):
        print(f"\n  [{i+1}] Incoming: \"{samples[i]['incoming'][:50]}\"")
        for source in all_results:
            r = all_results[source][i]
            label = source_names[source]
            print(f"    {label:>18}: \"{r['response'][:60]}\" ({r['scores']['overall']}/10)")

    output = {
        "aggregated": aggregated,
        "value_add": {
            "persona_over_raw": round(persona_overall - raw_overall, 1),
            "finetune_over_raw": round(ft_overall - raw_overall, 1),
            "gap_to_real": round(real_overall - max(persona_overall, ft_overall), 1),
        },
        "per_sample": {source: all_results[source] for source in all_results},
    }
    with open(OUTPUT_PATH, "w") as f:
        json.dump(output, f, indent=2)
    print(f"\n  Results saved to {OUTPUT_PATH}")


if __name__ == "__main__":
    main()
