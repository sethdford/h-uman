#!/usr/bin/env python3
"""
M4 — Real iMessage A/B: send prompts through the daemon's chat endpoint
(which uses the FULL agent_turn pipeline: persona injection, humor
framework, memory, world model, etc.) and verify responses are
Seth-voice via the shape classifier.

This is the direct empirical test the persona-eval audit chain has
been working toward. If responses are in-voice, U1 ("production has
persona injection") is empirically confirmed. If not, production has
a parallel bug.

Usage:
  ./build/human gateway --with-agent &
  python3 scripts/m4_production_ab.py
"""

import json
import sys
import time
from urllib import error, request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from eval_shape_classifier import classify  # noqa: E402

GATEWAY = "http://127.0.0.1:3006/v1/chat/completions"

# Same 8 iMessage prompts the eval suite uses, so direct comparison.
PROMPTS = [
    ("imsg-001", "[iMessage, close friend] Them: \"you around later?\"\nYou (one message, texting style):"),
    ("imsg-002", "[Group chat, 6 people] Someone: \"lol same\"\nYou (one line reaction):"),
    ("imsg-003", "[iMessage] Them: \"https://example.com/article — this is wild\"\nYou:"),
    ("imsg-006", "[iMessage] Them: \"did you send the doc to Alex yet? yes or no\"\nYou:"),
    ("imsg-008", "[iMessage, close friend] Them: \"ugh worst day, boss threw me under the bus in standup\"\nYou:"),
]


def post_chat(prompt: str, timeout_s: int = 120) -> tuple[str, float, str | None]:
    """Returns (content, elapsed_s, error). content is "" on error."""
    body = {
        "model": "mlx-community/gemma-4-26b-a4b-it-4bit",
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": 200,
        "temperature": 0.9,
    }
    data = json.dumps(body).encode("utf-8")
    req = request.Request(
        GATEWAY, data=data, method="POST",
        headers={"Content-Type": "application/json"},
    )
    t0 = time.time()
    try:
        with request.urlopen(req, timeout=timeout_s) as r:
            resp = json.loads(r.read())
        elapsed = time.time() - t0
        try:
            content = resp["choices"][0]["message"]["content"]
        except (KeyError, IndexError):
            return "", elapsed, f"malformed: {list(resp.keys())}"
        return content, elapsed, None
    except (error.URLError, error.HTTPError, json.JSONDecodeError) as e:
        return "", time.time() - t0, str(e)


def main():
    print("=" * 78)
    print("M4: PRODUCTION A/B — sending prompts through agent_turn-backed gateway")
    print("=" * 78)
    print()

    results = []
    for task_id, prompt in PROMPTS:
        print(f"### {task_id} ###")
        print(f"Prompt: {prompt[:120]}...")
        content, elapsed, err = post_chat(prompt)
        if err:
            print(f"  ERROR after {elapsed:.1f}s: {err}")
            results.append({"task_id": task_id, "content": "", "elapsed": elapsed, "error": err,
                             "shape": classify("")})
            print()
            continue
        shape = classify(content, channel="imessage")
        print(f"  Response ({len(content)} chars, {elapsed:.1f}s):")
        print(f"    {content[:200]!r}")
        print(f"  Shape: pass={shape['pass']} score={shape['score']} fails={shape['fails']}")
        results.append({"task_id": task_id, "content": content, "elapsed": elapsed,
                         "error": None, "shape": shape})
        print()

    # Aggregate
    print("=" * 78)
    print("AGGREGATE — Production (gateway with --with-agent) vs Eval (compact-prompt)")
    print("=" * 78)
    n = len(results)
    non_null = sum(1 for r in results if r["content"])
    shape_pass = sum(1 for r in results if r["shape"]["pass"])
    avg_score = sum(r["shape"]["score"] for r in results) / n if n else 0
    avg_len = sum(len(r["content"]) for r in results) / n if n else 0
    avg_elapsed = sum(r["elapsed"] for r in results) / n if n else 0
    print(f"  N tasks:         {n}")
    print(f"  Non-NULL:        {non_null}/{n} ({100*non_null/n:.1f}%)")
    print(f"  Shape pass:      {shape_pass}/{n} ({100*shape_pass/n:.1f}%)")
    print(f"  Mean shape:      {avg_score:.3f}")
    print(f"  Avg length:      {avg_len:.0f} chars")
    print(f"  Avg latency:     {avg_elapsed:.1f}s")
    print()
    print("Compare to eval (build/human eval run imessage_humanness.json compact-prompt era):")
    print("  Mean shape:      ~0.350 (post-fix) vs 0.053 (pre-fix)")
    print()
    if shape_pass / max(n, 1) >= 0.5:
        print("✅ Production produces in-voice responses — agent_turn persona injection WORKS.")
    else:
        print("⚠️  Production responses are NOT consistently in-voice.")
        print("    This would indicate a parallel bug to the eval-bypass; investigate.")

    # Save full results
    Path("/tmp/m4_production_ab.json").write_text(json.dumps(results, indent=2))
    print()
    print("Full results: /tmp/m4_production_ab.json")


if __name__ == "__main__":
    main()
