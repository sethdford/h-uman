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


def call(endpoint, model, ctx, a, b, temp, timeout):
    body = json.dumps({
        "model": model,
        "messages": [{"role": "system", "content": JUDGE_SYS},
                     {"role": "user", "content": judge_prompt(ctx, a, b)}],
        "temperature": temp, "max_tokens": 160,
    }).encode()
    req = urllib.request.Request(endpoint, data=body,
                                 headers={"Content-Type": "application/json"})
    r = json.load(urllib.request.urlopen(req, timeout=timeout))
    return r["choices"][0]["message"]["content"]


def parse(txt):
    m = re.search(r'\{.*\}', txt, re.S)
    if m:
        try:
            return json.loads(m.group(0))
        except Exception:
            pass
    mm = re.search(r'"?real"?\s*[:=]\s*"?([AB])', txt)
    return {"real": mm.group(1)} if mm else None


AXES = ["opinion", "memory", "reasoning", "lexical", "tone", "syntax"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet", help="rating_sheet.csv (with option_A/option_B)")
    ap.add_argument("--out", required=True, help="completed sheet for score.py")
    ap.add_argument("--endpoint", default="http://127.0.0.1:8741/v1/chat/completions")
    ap.add_argument("--model", default="gemma-4-31b-it-8bit")
    ap.add_argument("--temp", type=float, default=0.3)
    ap.add_argument("--timeout", type=int, default=120)
    a = ap.parse_args()

    rows = list(csv.DictReader(open(a.sheet)))
    if not rows:
        sys.exit("empty sheet")
    ok = fail = 0
    for i, r in enumerate(rows):
        try:
            j = parse(call(a.endpoint, a.model, r["context"], r["option_A"],
                           r["option_B"], a.temp, a.timeout))
        except Exception:
            j = None
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
