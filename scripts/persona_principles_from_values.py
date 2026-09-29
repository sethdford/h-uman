#!/usr/bin/env python3
"""Fill an empty `core.principles` from the persona's own `core.values`.

seth.json validated on 2026-09-20 with `core.principles: []` — the only empty
field in the file, empty in every backup, never authored. Rather than invent
principles for a real person, derive them from what the persona already
asserts: the `values` list (18 entries, produced by the analyzer from Seth's
own messages), the identity paragraph, and `decision_style`.

Uses the local OpenAI-compatible server (default http://127.0.0.1:8741, the
production GLM-4.5-Air serving path) so nothing leaves the machine. Output is
6–8 first-person operating principles, each <= 14 words, each traceable to a
listed value. Default is dry-run; --write backs up to
seth.json.bak-principles-<ts> and replaces only `core.principles`.

The change is prompt-visible, so it is gated the same way any voice change
is: run scripts/eval_blinded_ab.py afterwards; regress past its ENFORCING
thresholds → restore the backup.

Usage: scripts/persona_principles_from_values.py [--write] [--server URL]
"""
import argparse
import json
import os
import sys
import time
import urllib.request

PERSONA = os.path.expanduser("~/.human/personas/seth.json")

PROMPT = """You are helping fill in a persona file for a texting assistant that speaks AS the person described.
Write the person's operating principles: the rules they actually live by, in their own first-person voice.

Constraints:
- Output ONLY a JSON array of 6 to 8 strings. No prose, no keys, no markdown.
- Each principle is one sentence, at most 14 words, plain and concrete.
- Every principle must follow from at least one listed value; do not add values that are not listed.
- Do not mention the company, the city, or family members by name.

Identity: {identity}
Decision style: {decision_style}
Values: {values}
"""


def derive(server, identity, decision_style, values, model=None, timeout=180):
    body = {
        "messages": [{"role": "user", "content": PROMPT.format(
            identity=identity[:600], decision_style=decision_style, values=", ".join(values))}],
        "temperature": 0.2,
        "max_tokens": 400,
    }
    if model:
        body["model"] = model
    req = urllib.request.Request(f"{server.rstrip('/')}/v1/chat/completions",
                                 data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        text = json.load(r)["choices"][0]["message"]["content"]
    return parse_principles(text)


def parse_principles(text):
    """Accept a bare JSON array, or one wrapped in code fences / prose."""
    start, end = text.find("["), text.rfind("]")
    if start < 0 or end <= start:
        raise ValueError(f"no JSON array in model output: {text[:200]!r}")
    arr = json.loads(text[start:end + 1])
    out = [s.strip() for s in arr if isinstance(s, str) and s.strip()]
    if not 6 <= len(out) <= 8:
        raise ValueError(f"expected 6-8 principles, got {len(out)}")
    too_long = [s for s in out if len(s.split()) > 14]
    if too_long:
        raise ValueError(f"principle over 14 words: {too_long[0]!r}")
    return out


def write_persona(persona_path, principles):
    with open(persona_path, encoding="utf-8") as f:
        persona = json.load(f)
    backup = f"{persona_path}.bak-principles-{int(time.time())}"
    with open(persona_path, "rb") as src, open(backup, "wb") as dst:
        dst.write(src.read())
    persona["core"]["principles"] = principles
    tmp = persona_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(persona, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, persona_path)
    return backup


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--persona", default=PERSONA)
    ap.add_argument("--server", default=os.environ.get("HU_LOCAL_SERVER", "http://127.0.0.1:8741"))
    ap.add_argument("--model", default=None)
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args(argv)

    with open(args.persona, encoding="utf-8") as f:
        core = json.load(f)["core"]
    if core.get("principles"):
        print(f"core.principles already has {len(core['principles'])} entries; refusing to overwrite",
              file=sys.stderr)
        return 2
    principles = derive(args.server, core.get("identity", ""), core.get("decision_style", ""),
                        core.get("values", []), model=args.model)
    print(json.dumps(principles, indent=2, ensure_ascii=False))
    if args.write:
        print(f"wrote {args.persona} (backup: {write_persona(args.persona, principles)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
