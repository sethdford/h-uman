#!/usr/bin/env python3
"""Golden-set evaluation for Human agent. Runs test cases and optionally uses Gemini for quality scoring."""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def load_golden_set(path: str) -> list:
    cases = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                cases.append(json.loads(line))
    return cases


def run_one_shot(binary: str, input_text: str, timeout: int = 60) -> tuple:
    """Run human with -m/--message for one-shot response. Returns (stdout, success)."""
    try:
        result = subprocess.run(
            [binary, "-m", input_text],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return result.stdout or result.stderr or "", result.returncode == 0
    except subprocess.TimeoutExpired:
        return "", False
    except Exception:
        return "", False


def smoke_check(response: str, expected_topics: list, min_relevance: int) -> dict:
    """Basic smoke test when no Gemini key: non-empty, reasonable length, topic coverage."""
    passed = True
    notes = []
    if not response or not response.strip():
        passed = False
        notes.append("empty response")
    elif len(response) < 10:
        passed = False
        notes.append("response too short")
    elif len(response) > 50000:
        notes.append("response very long")
    found = sum(1 for t in expected_topics if t.lower() in response.lower())
    if found == 0:
        passed = False
        notes.append("no expected topics found")
    return {
        "relevance": min(10, max(1, found * 2)) if expected_topics else 5,
        "tone": 5,
        "factual": 5,
        "passed": passed,
        "notes": notes,
    }


def gemini_eval(response: str, input_text: str, expected_topics: list) -> dict | None:
    """Call Gemini API to score response. Returns dict with relevance, tone, factual or None on failure."""
    key = os.environ.get("GEMINI_EVAL_KEY")
    if not key:
        return None
    try:
        import urllib.request

        prompt = (
            f"Score this AI response (1-10) for relevance, tone, and factual accuracy.\n"
            f"Input: {input_text}\n"
            f"Expected topics (optional): {expected_topics}\n"
            f"Response: {response[:2000]}\n"
            f"Return JSON: {{\"relevance\": N, \"tone\": N, \"factual\": N}}"
        )
        # Structured output: constrain the judge to bare JSON instead of
        # fence-stripping free text (see eval_humanness.py reference pattern).
        score_schema = {
            "type": "object",
            "properties": {
                "relevance": {"type": "integer", "minimum": 1, "maximum": 10},
                "tone": {"type": "integer", "minimum": 1, "maximum": 10},
                "factual": {"type": "integer", "minimum": 1, "maximum": 10},
            },
            "required": ["relevance", "tone", "factual"],
            "propertyOrdering": ["relevance", "tone", "factual"],
        }
        body = json.dumps({
            "contents": [{"parts": [{"text": prompt}]}],
            "generationConfig": {
                "temperature": 0,
                "maxOutputTokens": 64,
                "responseMimeType": "application/json",
                "responseSchema": score_schema,
            },
        }).encode()
        req = urllib.request.Request(
            f"https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent?key={key}",
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read().decode())
        text = data.get("candidates", [{}])[0].get("content", {}).get("parts", [{}])[0].get("text", "{}")
        if "```" in text:
            text = text.split("```")[1].replace("json", "").strip()
        scores = json.loads(text)
        return {
            "relevance": min(10, max(1, scores.get("relevance", 5))),
            "tone": min(10, max(1, scores.get("tone", 5))),
            "factual": min(10, max(1, scores.get("factual", 5))),
            "passed": scores.get("relevance", 5) >= 5,
            "notes": [],
        }
    except Exception:
        return None


def main() -> None:
    parser = argparse.ArgumentParser(description="Golden-set evaluation for Human agent")
    parser.add_argument("--binary", default="./build/human", help="Path to human binary")
    parser.add_argument("--golden-set", default="data/eval/golden_set.jsonl", help="Path to JSONL golden set")
    parser.add_argument("--output", default="eval_results.json", help="Output JSON path")
    parser.add_argument("--timeout", type=int, default=60, help="Timeout per case (seconds)")
    args = parser.parse_args()

    if not Path(args.binary).exists():
        print(f"Binary not found: {args.binary}", file=sys.stderr)
        sys.exit(1)
    if not Path(args.golden_set).exists():
        print(f"Golden set not found: {args.golden_set}", file=sys.stderr)
        sys.exit(1)

    cases = load_golden_set(args.golden_set)
    results = []
    for case in cases:
        case_id = case.get("id", "unknown")
        input_text = case.get("input", "")
        expected_topics = case.get("expected_topics", [])
        min_relevance = case.get("min_relevance", 5)

        response, ok = run_one_shot(args.binary, input_text, args.timeout)
        scores = gemini_eval(response, input_text, expected_topics)
        if scores is None:
            scores = smoke_check(response, expected_topics, min_relevance)

        results.append({
            "id": case_id,
            "input": input_text,
            "response_length": len(response),
            "success": ok,
            "relevance": scores["relevance"],
            "tone": scores["tone"],
            "factual": scores["factual"],
            "passed": scores["passed"],
            "notes": scores.get("notes", []),
        })

    avg_relevance = sum(r["relevance"] for r in results) / len(results) if results else 0
    pass_rate = sum(1 for r in results if r["passed"]) / len(results) if results else 0

    output = {
        "summary": {"avg_relevance": avg_relevance, "pass_rate": pass_rate, "total": len(results)},
        "results": results,
    }
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(output, f, indent=2)
    print(f"Wrote {args.output}: avg_relevance={avg_relevance:.2f}, pass_rate={pass_rate:.2f}")


if __name__ == "__main__":
    main()
