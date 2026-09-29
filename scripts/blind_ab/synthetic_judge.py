#!/usr/bin/env python3
"""Synthetic LLM-judge for the blind A/B INNER LOOP. For each item, asks a model
"which reply is the real person, A or B?" plus six-axis (1-5) scores, and writes a
COMPLETED rating sheet that score.py scores exactly like a human rater — but fast,
free, and repeatable on every change.

PRIVACY: calls a LOCAL endpoint by default (127.0.0.1) so the real messages never
leave the machine. Do not point --endpoint at a cloud API with real data.

VALIDITY CAVEAT (read this before trusting the number):
A local judge shares the GENERATOR's model family, so it over-rates the AI's own
distribution ("shared blindspot"). Therefore:
  - a HIGH synthetic detect-rate is MEANINGFUL  -> if even a same-family model can
    tell them apart, humans certainly can: that's a real FAIL signal.
  - a LOW synthetic detect-rate is OPTIMISTIC    -> needs a human pass to certify.
Use this as the development/regression signal; certify "indistinguishable" with
humans who know the person (see RATER-KIT-README.md). Best practice: judge with a
DIFFERENT model family than the generator to reduce the shared blindspot.
"""
import argparse, csv, json, re, sys, urllib.request

JUDGE_SYS = ("You evaluate whether a text-message reply was written by a real "
             "specific person or by an AI imitating their texting style. Be a "
             "discerning, skeptical judge.")


def judge_prompt(ctx, a, b):
    return (f'Incoming message: "{ctx}"\n\n'
            f'Reply A: "{a}"\nReply B: "{b}"\n\n'
            'Exactly one reply was written by the REAL person; the other by an AI '
            'imitating them. Reply with STRICT JSON only, no prose:\n'
            '{"real":"A"|"B","opinion":1-5,"memory":1-5,"reasoning":1-5,'
            '"lexical":1-5,"tone":1-5,"syntax":1-5}\n'
            'The 1-5 axis scores rate how natural/human the reply you judged REAL '
            'is on each dimension (5=indistinguishably human).')


def call(api, endpoint, model, ctx, a, b, temp, timeout):
    prompt = judge_prompt(ctx, a, b)
    if api == "anthropic":
        # CLOUD judge (milestone tier). Ships content to Anthropic — only run with
        # explicit consent; conflicts with the local-first privacy thesis.
        import os
        key = os.environ.get("ANTHROPIC_API_KEY")
        if not key:
            raise RuntimeError("ANTHROPIC_API_KEY not set")
        body = json.dumps({
            "model": model, "max_tokens": 160, "temperature": temp,
            "system": JUDGE_SYS,
            "messages": [{"role": "user", "content": prompt}],
        }).encode()
        req = urllib.request.Request(endpoint, data=body, headers={
            "content-type": "application/json", "x-api-key": key,
            "anthropic-version": "2023-06-01"})
        r = json.load(urllib.request.urlopen(req, timeout=timeout))
        return r["content"][0]["text"]
    # default: OpenAI-compatible (local mlx-server, gemma — privacy-preserving)
    # max_tokens must cover gemma's THINKING channel + the answer: stock
    # mlx_lm server bills reasoning against the same budget, and at 160 the
    # model burned it all reasoning — finish_reason=length with NO content
    # key at all (2026-07-25, the thinking-budget gotcha, stock-server form).
    body = json.dumps({
        "model": model,
        "messages": [{"role": "system", "content": JUDGE_SYS},
                     {"role": "user", "content": prompt}],
        "temperature": temp, "max_tokens": 1024,
    }).encode()
    req = urllib.request.Request(endpoint, data=body,
                                 headers={"Content-Type": "application/json"})
    r = json.load(urllib.request.urlopen(req, timeout=timeout))
    msg = r["choices"][0]["message"]
    # Stock mlx_lm server splits thinking into message.reasoning and may omit
    # "content" entirely on a length cut. Prefer the visible answer; fall back
    # to scanning the reasoning (parse() anchors to the LAST JSON envelope, so
    # the schema echo that always appears early in reasoning can't win).
    return msg.get("content") or msg.get("reasoning") or ""


def parse(txt):
    # Prefer the LAST valid JSON object carrying a "real" verdict: gemma's
    # thinking channel ECHOES the answer schema ('{"real":"A"|"B",...}' — not
    # valid JSON) before the actual verdict, so a first-match (or greedy-span)
    # scan harvests the echo, not the answer. Same trap and same fix as the
    # LLM fact extractor's prefer-LAST-envelope parse (2026-07-11).
    for m in reversed(list(re.finditer(r'\{[^{}]*\}', txt, re.S))):
        try:
            j = json.loads(m.group(0))
        except Exception:
            continue
        if isinstance(j, dict) and j.get("real") in ("A", "B"):
            return j
    # (?!"?\s*\|) rejects the schema echo's alternation form '"A"|"B"'.
    matches = list(re.finditer(r'"?real"?\s*[:=]\s*"?([AB])(?!"?\s*\|)', txt))
    return {"real": matches[-1].group(1)} if matches else None


AXES = ["opinion", "memory", "reasoning", "lexical", "tone", "syntax"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet", help="rating_sheet.csv (with option_A/option_B)")
    ap.add_argument("--out", required=True, help="completed sheet for score.py")
    ap.add_argument("--api", choices=["openai", "anthropic"], default="openai",
                    help="openai=local mlx-server (private, default); "
                         "anthropic=cloud Opus milestone judge (ships data — consent required)")
    ap.add_argument("--endpoint", default=None)
    ap.add_argument("--model", default=None)
    ap.add_argument("--temp", type=float, default=0.3)
    ap.add_argument("--timeout", type=int, default=120)
    a = ap.parse_args()
    # Resolve per-api defaults (overridable).
    if a.api == "anthropic":
        a.endpoint = a.endpoint or "https://api.anthropic.com/v1/messages"
        a.model = a.model or "claude-opus-4-20250514"
        a.timeout = max(a.timeout, 60)
        print("WARNING: --api anthropic ships message content to the cloud "
              "(against the local-first privacy thesis). Ctrl-C now if not intended.",
              file=sys.stderr)
    else:
        a.endpoint = a.endpoint or "http://127.0.0.1:8741/v1/chat/completions"
        a.model = a.model or "gemma-4-31b-it-8bit"

    rows = list(csv.DictReader(open(a.sheet)))
    if not rows:
        sys.exit("empty sheet")
    ok = fail = 0
    for i, r in enumerate(rows):
        try:
            j = parse(call(a.api, a.endpoint, a.model, r["context"], r["option_A"],
                           r["option_B"], a.temp, a.timeout))
        except Exception:
            j = None
        # Stamp the judging model + family on every row so the downstream
        # reward wire (judge_to_dpo.py) can enforce the different-family-only
        # gate: same-family (local gemma) verdicts inflate via shared blindspot
        # and MUST NOT become training pairs. (Empirically: gemma self-judge
        # 0.21 vs Opus 0.90 on the same triples.)
        r["judge_api"] = a.api
        r["judge_model"] = a.model
        if j and j.get("real") in ("A", "B"):
            ok += 1
            r["choice"] = j["real"]
            r["confidence"] = "3"
            for ax in AXES:
                if ax in j:
                    r[f"axis_{ax}"] = j[ax]
        else:
            fail += 1
            r["choice"] = ""  # blank -> score.py skips it
        print(f"  judged {i+1}/{len(rows)} (ok={ok} parse_fail={fail})",
              file=sys.stderr, flush=True)

    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)
    print(f"SYNTHJUDGE_DONE judged_ok={ok} parse_fail={fail} -> {a.out}")


if __name__ == "__main__":
    main()
