#!/usr/bin/env python3
"""
SOTA experiment: bare-prompt vs persona-wrapped prompt vs ground-truth example.

For each task in eval_suites/imessage_humanness.json, send the same prompt
to MLX three ways:

  A. BARE — no system prompt, just the eval prompt as a user message
  B. PERSONA-WRAPPED — system prompt built from seth.json's identity +
     iMessage overlay + humor + anti-patterns + 5 example shots
  C. ECHO — just shows what Seth's actual real iMessage style looks like
     (from example_banks) as a sanity check

Records: response text, response length, contains-markdown flag, score per
the eval's rubric (LLM-judge call via the same provider). Prints a
side-by-side diff per task.

This is the controlled experiment that proves whether the model produces
"AI assistant" output because (A) it inherently does so, or (B) it does so
ONLY when persona context is missing.

Per the 2026-05-18 audit: if B's responses are dramatically shorter, lack
markdown, and pass the rubric while A fails, the diagnosis is "eval path
bypasses persona". The fix is then to route eval through agent_turn.
"""

import json
import os
import re
import sys
import time
from urllib import request, error
from pathlib import Path

MLX_URL = "http://127.0.0.1:8741/v1/chat/completions"
PERSONA_PATH = Path.home() / ".human" / "personas" / "seth.json"
SUITE_PATH = Path(__file__).parent.parent / "eval_suites" / "imessage_humanness.json"


def build_persona_system_prompt(persona: dict, channel: str = "imessage") -> str:
    """Mirror what agent_turn.c builds (approximately) — identity + overlay
    + humor + anti-patterns + example shots."""
    parts = []

    # 1. Identity
    identity = (persona.get("core") or {}).get("identity") or ""
    if identity:
        parts.append(f"# You are\n\n{identity}")

    # 2. Channel overlay
    overlay = (persona.get("channel_overlays") or {}).get(channel) or {}
    if overlay:
        formality = overlay.get("formality", "")
        avg_length = overlay.get("avg_length", "")
        emoji_usage = overlay.get("emoji_usage", "")
        notes = overlay.get("style_notes", [])
        ov_parts = [f"# Channel: {channel}"]
        if formality:
            ov_parts.append(f"- Formality: {formality}")
        if avg_length:
            ov_parts.append(f"- Length: {avg_length}")
        if emoji_usage:
            ov_parts.append(f"- Emoji: {emoji_usage}")
        if notes:
            ov_parts.append("- Style notes:")
            for n in notes[:10]:
                ov_parts.append(f"  - {n}")
        parts.append("\n".join(ov_parts))

    # 3. Anti-patterns (THE critical constraint)
    anti = persona.get("anti_patterns") or []
    if anti:
        ap = ["# Hard rules — NEVER do these:"]
        for a in anti[:12]:
            ap.append(f"- {a}")
        parts.append("\n".join(ap))

    # 4. Style rules
    style = persona.get("style_rules") or []
    if style:
        sp = ["# Style rules"]
        for s in style[:10]:
            sp.append(f"- {s}")
        parts.append("\n".join(sp))

    # 5. Humor
    humor = persona.get("humor") or {}
    if humor:
        hp = ["# Humor"]
        hp.append(f"- Style: {', '.join(humor.get('style', [])[:5])}")
        hp.append(f"- Frequency: {humor.get('frequency', '')}")
        hp.append("- Examples of YOUR humor (incoming -> your response):")
        for ex in (humor.get("examples") or [])[:6]:
            hp.append(f"  - {ex}")
        parts.append("\n".join(hp))

    # 6. Example shots from iMessage bank
    banks = persona.get("example_banks") or []
    imsg_bank = next((b for b in banks if b.get("channel") == "imessage"), {})
    examples = imsg_bank.get("examples", [])[:10]
    if examples:
        ep = ["# Examples of how YOU actually text on iMessage (real history)"]
        for ex in examples:
            inc = ex.get("incoming", "")[:200]
            resp = ex.get("response", "")[:200]
            ep.append(f"- Them: {inc!r}")
            ep.append(f"  You: {resp!r}")
        parts.append("\n".join(ep))

    # 7. Voice rhythm
    vr = persona.get("voice_rhythm") or {}
    if vr:
        parts.append(
            "# Voice rhythm\n"
            + json.dumps(vr, indent=2)[:600]
        )

    parts.append(
        "\n# Output\nRespond as Seth would actually text. ONE message. "
        "No markdown. No bullet lists. No 'options'. No 'Depending on'. "
        "Just the text you'd actually send."
    )

    return "\n\n".join(parts)


def post_chat(system_prompt: str | None, user_prompt: str, max_tokens: int = 200,
              temperature: float = 0.9) -> tuple[str, float]:
    """POST a chat completion to MLX. Returns (content, elapsed_seconds)."""
    messages = []
    if system_prompt:
        messages.append({"role": "system", "content": system_prompt})
    messages.append({"role": "user", "content": user_prompt})
    body = {
        "model": "gemma-4-31b-seth-v3-fused",
        "messages": messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
    }
    data = json.dumps(body).encode("utf-8")
    req = request.Request(
        MLX_URL,
        data=data,
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    t0 = time.time()
    try:
        with request.urlopen(req, timeout=120) as r:
            resp = json.loads(r.read())
    except (error.URLError, error.HTTPError) as e:
        return f"[error: {e}]", time.time() - t0
    elapsed = time.time() - t0
    try:
        content = resp["choices"][0]["message"]["content"]
    except (KeyError, IndexError):
        content = f"[malformed response: {resp!r}]"
    return content, elapsed


def has_markdown(text: str) -> bool:
    """Detect bullet lists, numbered lists, headers, bold markdown."""
    patterns = [
        r"^\s*[\*\-]\s+",  # bullet
        r"^\s*\d+\.\s+",  # numbered
        r"^#{1,6}\s+",  # header
        r"\*\*[^\*]+\*\*",  # bold
        r"^---+\s*$",  # divider
    ]
    return any(re.search(p, text, re.MULTILINE) for p in patterns)


def main():
    persona = json.loads(PERSONA_PATH.read_text())
    suite = json.loads(SUITE_PATH.read_text())
    persona_sys = build_persona_system_prompt(persona)

    print(f"=== Persona system prompt: {len(persona_sys)} chars ===")
    print(f"=== Suite: {suite['name']} ({len(suite['tasks'])} tasks) ===")
    print()

    results = []
    for task in suite["tasks"]:
        tid = task.get("id", "?")
        prompt = task.get("prompt", "")
        print(f"### {tid} ###")
        print(f"Prompt: {prompt[:120]}...")
        print()

        # A. BARE
        bare_resp, bare_t = post_chat(None, prompt, max_tokens=300)
        bare_md = has_markdown(bare_resp)
        bare_len = len(bare_resp)
        print(f"A. BARE (no system): {bare_len} chars, markdown={bare_md}, {bare_t:.1f}s")
        print(f"   {bare_resp[:200]!r}")
        print()

        # B. PERSONA-WRAPPED
        persona_resp, persona_t = post_chat(persona_sys, prompt, max_tokens=300)
        persona_md = has_markdown(persona_resp)
        persona_len = len(persona_resp)
        print(f"B. PERSONA-WRAPPED: {persona_len} chars, markdown={persona_md}, {persona_t:.1f}s")
        print(f"   {persona_resp[:200]!r}")
        print()

        results.append({
            "task_id": tid,
            "bare": {"len": bare_len, "markdown": bare_md, "text": bare_resp},
            "persona": {"len": persona_len, "markdown": persona_md, "text": persona_resp},
        })
        print("-" * 80)
        print()

    # Aggregate
    print("=" * 80)
    print("AGGREGATE")
    print("=" * 80)
    bare_md_count = sum(1 for r in results if r["bare"]["markdown"])
    persona_md_count = sum(1 for r in results if r["persona"]["markdown"])
    bare_avg_len = sum(r["bare"]["len"] for r in results) / len(results)
    persona_avg_len = sum(r["persona"]["len"] for r in results) / len(results)
    print(f"Bare:    avg_len={bare_avg_len:.0f} chars, markdown_count={bare_md_count}/{len(results)}")
    print(f"Persona: avg_len={persona_avg_len:.0f} chars, markdown_count={persona_md_count}/{len(results)}")
    print()
    print(f"Length reduction with persona: {(1 - persona_avg_len/bare_avg_len)*100:.0f}%")
    print(f"Markdown reduction with persona: {bare_md_count - persona_md_count} fewer tasks")

    # Save to JSON for later analysis
    out = Path("/tmp/persona_eval_comparison.json")
    out.write_text(json.dumps(results, indent=2))
    print()
    print(f"Full results: {out}")


if __name__ == "__main__":
    main()
