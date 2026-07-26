#!/usr/bin/env python3
"""Head-to-head smoke test of two LoRA adapters on the same base model.

Implements the pre-promotion gate required by
.claude/rules/lora-scale-default-or-die.md: a persona adapter must be checked
on THREE categories before it is fused or served —

    persona      voice / style            (human judgement; shown side by side)
    instruction  base instruction-following   (AUTOMATED pass/fail)
    reasoning    multi-turn reasoning         (AUTOMATED pass/fail)

The two base categories are objectively checkable, so a base-capability
REGRESSION is detected automatically rather than eyeballed. That is the failure
this gate exists for: an adapter can nail the persona while destroying the
base's ability to follow instructions, and validation loss will not show it
(the 2026-05-25 scale=20.0 incident produced exactly that, and shipped zero
usable replies for ~2 weeks).

Also reports the LEADING-BYTE ARTIFACT rate per adapter. v5 was trained on a
corpus that was 18.8% corrupted on the target side (a typedstream decoder
leaked a length byte as text), so it was taught to emit a stray leading
character. v6 is the same recipe on the fixed corpus. A drop in this rate is
direct evidence the corpus fix reached the model.

Memory: ONE adapter is resident at a time (two 31B models would be ~73 GB).
Generation is temp=0.0 through the tokenizer's chat template, matching
scripts/eval_fidelity_nightly.py so numbers are comparable.

Usage:
    python3 adapter_smoke_test.py --a <v5_adapter_path> --b <v6_adapter_path> \
        --out /tmp/smoke.json

    python3 adapter_smoke_test.py --selftest      # checkers only, no model load
"""
import argparse
import gc
import json
import os
import re
import sys

DEFAULT_BASE = "mlx-community/gemma-4-31b-it-8bit"
SYSTEM = ("You are Seth Ford, 45, texting on iMessage. Chief Architect at "
          "Vanguard. Reply the way Seth actually texts.")

# Reuse the corruption predicate rather than copying it — duplicating this
# exact decoder-adjacent logic across files is what caused the 20% corpus
# corruption in the first place (three divergent copies, one fix).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from binoculars_to_dpo import looks_corrupted  # noqa: E402


# ---------------------------------------------------------------- prompts ---
# expect: regex the reply MUST match for the check to pass (base categories).
# persona prompts have no expect — voice is a human call.
PROMPTS = [
    # --- persona: voice. system prompt applied. ---
    {"id": "p1", "cat": "persona", "msgs": [{"role": "user", "content": "How’s ur morning??"}]},
    {"id": "p2", "cat": "persona", "msgs": [{"role": "user", "content": "you around this weekend?"}]},
    {"id": "p3", "cat": "persona", "msgs": [{"role": "user", "content": "hey did you see the game last night"}]},
    {"id": "p4", "cat": "persona", "msgs": [{"role": "user", "content": "sorry running late"}]},
    {"id": "p5", "cat": "persona", "msgs": [{"role": "user", "content": "how's the new job going"}]},
    {"id": "p6", "cat": "persona", "msgs": [{"role": "user", "content": "can you send me that doc when you get a sec"}]},

    # --- instruction: base capability. no persona system prompt. ---
    {"id": "i1", "cat": "instruction", "system": None, "expect": r"\b115\b",
     "msgs": [{"role": "user", "content": "What is 47 + 68? Reply with just the number."}]},
    {"id": "i2", "cat": "instruction", "system": None, "expect": r"chat",
     "msgs": [{"role": "user", "content": "Translate to French: 'the cat is on the table'"}]},
    {"id": "i3", "cat": "instruction", "system": None, "expect": r"1856",
     "msgs": [{"role": "user", "content": "Extract the year from this sentence: 'The treaty was signed in 1856 near Paris.'"}]},
    {"id": "i4", "cat": "instruction", "system": None, "expect": r"\bblue\b",
     "msgs": [{"role": "user", "content": "Reply with exactly one word. What color is a clear midday sky?"}]},
    {"id": "i5", "cat": "instruction", "system": None, "expect": r"(?s)red.*blue|blue.*red",
     "msgs": [{"role": "user", "content": "Name three primary colors, comma-separated, nothing else."}]},
    {"id": "i6", "cat": "instruction", "system": None, "expect": r"^\s*\{.*\}\s*$",
     "msgs": [{"role": "user", "content": 'Reply with ONLY a JSON object, no prose, with keys "a" and "b" set to 1 and 2.'}]},

    # --- reasoning: multi-turn + inference. no persona system prompt. ---
    {"id": "r1", "cat": "reasoning", "system": None, "expect": r"\b7\b",
     "msgs": [{"role": "user", "content": "I have 3 apples."},
              {"role": "assistant", "content": "Got it, you have 3 apples."},
              {"role": "user", "content": "I ate one, then bought 5 more. How many do I have now? Answer with the number."}]},
    {"id": "r2", "cat": "reasoning", "system": None, "expect": r"\byes\b",
     "msgs": [{"role": "user", "content": "All bloops are razzies. All razzies are lazzies. Are all bloops lazzies? Answer yes or no."}]},
    {"id": "r3", "cat": "reasoning", "system": None, "expect": r"Dana",
     "msgs": [{"role": "user", "content": "My name is Dana and I live in Denver."},
              {"role": "assistant", "content": "Nice to meet you, Dana."},
              {"role": "user", "content": "What is my name?"}]},
    {"id": "r4", "cat": "reasoning", "system": None, "expect": r"\b(12|twelve)\b",
     "msgs": [{"role": "user", "content": "A train leaves at 9am and arrives at 9pm the same day. How many hours is the trip? Answer with the number."}]},
]


# --------------------------------------------------------------- checkers ---

def is_degenerate(text, min_reps=4):
    """True if the reply collapses into a repeated token/phrase runaway."""
    words = text.split()
    for n in (1, 2, 3):
        if len(words) < n * min_reps:   # not enough words for n reps of an n-gram
            continue
        for i in range(len(words) - n * min_reps + 1):
            gram = words[i:i + n]
            if all(words[i + k * n: i + (k + 1) * n] == gram for k in range(min_reps)):
                return True
    return False


ERROR_MARKERS = ("<error", "GENERATE_ERROR", "TIMEOUT", "[ERROR")


def evaluate(entry, text):
    """Per-response checks. Returns (passed, list_of_failure_tags)."""
    t = (text or "").strip()
    fails = []
    if not t or any(m in t for m in ERROR_MARKERS):
        fails.append("empty_or_error")
    if t and is_degenerate(t):
        fails.append("degenerate")
    if t and looks_corrupted(t):
        fails.append("leading_artifact")
    exp = entry.get("expect")
    if exp and not re.search(exp, t, re.IGNORECASE | re.MULTILINE):
        fails.append("expectation_missed")
    return (not fails), fails


# ------------------------------------------------------------- generation ---

def run_adapter(base, adapter_path, label, max_tokens, quiet=False):
    """Load base+adapter, generate every prompt, free the model."""
    import mlx.core as mx
    from mlx_lm import load, generate as mlx_generate
    from mlx_lm.sample_utils import make_sampler

    if not quiet:
        print(f"[{label}] loading {base} + {adapter_path}", file=sys.stderr)
    model, tokenizer = load(base, adapter_path=adapter_path)
    sampler = make_sampler(temp=0.0)  # deterministic, matches eval_fidelity_nightly

    out = {}
    for e in PROMPTS:
        msgs = list(e["msgs"])
        system = e.get("system", SYSTEM) if "system" in e else SYSTEM
        if system:
            msgs = [{"role": "system", "content": system}] + msgs
        prompt = tokenizer.apply_chat_template(msgs, add_generation_prompt=True)
        try:
            text = mlx_generate(model, tokenizer, prompt=prompt,
                                max_tokens=max_tokens, sampler=sampler)
        except Exception as ex:  # a crash on one prompt shouldn't kill the run
            text = f"[ERROR {type(ex).__name__}: {ex}]"
        out[e["id"]] = (text or "").strip()
        if not quiet:
            print(f"[{label}] {e['id']} ok", file=sys.stderr)
        mx.clear_cache()

    del model
    gc.collect()
    mx.clear_cache()
    return out


# ---------------------------------------------------------------- report ----

def build_report(res_a, res_b, label_a, label_b):
    rows, cats = [], {}
    for e in PROMPTS:
        pa, fa = evaluate(e, res_a.get(e["id"], ""))
        pb, fb = evaluate(e, res_b.get(e["id"], ""))
        rows.append({"id": e["id"], "cat": e["cat"],
                     "prompt": e["msgs"][-1]["content"],
                     label_a: {"text": res_a.get(e["id"], ""), "pass": pa, "fails": fa},
                     label_b: {"text": res_b.get(e["id"], ""), "pass": pb, "fails": fb}})
        c = cats.setdefault(e["cat"], {"n": 0, label_a: 0, label_b: 0})
        c["n"] += 1
        c[label_a] += int(pa)
        c[label_b] += int(pb)

    # A base-capability check that A passes and B fails is a REGRESSION.
    regressions = [r for r in rows if r["cat"] in ("instruction", "reasoning")
                   and r[label_a]["pass"] and not r[label_b]["pass"]]
    fixes = [r for r in rows if r["cat"] in ("instruction", "reasoning")
             and not r[label_a]["pass"] and r[label_b]["pass"]]
    art = {lbl: sum(1 for r in rows if "leading_artifact" in r[lbl]["fails"])
           for lbl in (label_a, label_b)}
    return {"categories": cats, "regressions": regressions, "fixes": fixes,
            "leading_artifact_counts": art, "rows": rows}


def print_report(rep, label_a, label_b):
    print("\n" + "=" * 74)
    print(f"ADAPTER SMOKE TEST   A={label_a}   B={label_b}")
    print("=" * 74)
    print(f"\n{'category':<14}{'n':>4}{'  '+label_a:>16}{'  '+label_b:>16}")
    for cat, c in rep["categories"].items():
        note = "  (voice — human call)" if cat == "persona" else ""
        print(f"{cat:<14}{c['n']:>4}{c[label_a]:>16}{c[label_b]:>16}{note}")

    a_art = rep["leading_artifact_counts"][label_a]
    b_art = rep["leading_artifact_counts"][label_b]
    print(f"\nleading-byte artifact:  {label_a}={a_art}   {label_b}={b_art}"
          "   (corpus-fix signal; lower is better)")

    if rep["regressions"]:
        print(f"\n  *** BASE-CAPABILITY REGRESSIONS ({len(rep['regressions'])}) — DO NOT PROMOTE ***")
        for r in rep["regressions"]:
            print(f"    [{r['cat']}/{r['id']}] {r['prompt'][:56]}")
            print(f"        {label_a} PASS: {r[label_a]['text'][:60]!r}")
            print(f"        {label_b} FAIL {r[label_b]['fails']}: {r[label_b]['text'][:60]!r}")
    else:
        print("\n  No base-capability regressions.")
    if rep["fixes"]:
        print(f"\n  Improvements ({len(rep['fixes'])}): "
              + ", ".join(f"{r['cat']}/{r['id']}" for r in rep["fixes"]))

    print("\n--- persona replies, side by side (judge voice yourself) ---")
    for r in rep["rows"]:
        if r["cat"] != "persona":
            continue
        print(f"\n  > {r['prompt']}")
        print(f"    {label_a}: {r[label_a]['text'][:100]!r}")
        print(f"    {label_b}: {r[label_b]['text'][:100]!r}")

    verdict = "BLOCKED — base-capability regression" if rep["regressions"] \
        else "NO BLOCKER — persona quality is a human call"
    print(f"\nVERDICT: {verdict}")
    print("Promotion also requires recalibrating the Binoculars thresholds "
          "(docs/research/2026-07-25-binoculars-discriminator.md).\n")
    return not rep["regressions"]


# -------------------------------------------------------------- selftest ----

def selftest():
    assert is_degenerate("yo yo yo yo yo")
    assert is_degenerate("ha ha ha ha ha ha")
    assert not is_degenerate("hey what's up, running a bit late but on my way")
    assert not is_degenerate("")
    assert is_degenerate("go now go now go now go now")

    e = {"expect": r"\b115\b"}
    assert evaluate(e, "115")[0]
    assert evaluate(e, "The answer is 115.")[0]
    assert evaluate(e, "116")[1] == ["expectation_missed"]
    assert "empty_or_error" in evaluate(e, "")[1]
    assert "empty_or_error" in evaluate(e, "[ERROR Foo: bar]")[1]
    # leading-byte artifact is caught even when the expectation passes
    ok, fails = evaluate({"expect": r"morning"}, ",Good morning")
    assert not ok and "leading_artifact" in fails, fails
    # persona entry (no expect) passes on ordinary text
    assert evaluate({}, "yeah on my way")[0]

    a = {"i1": "115", "r1": "7", "p1": "hey"}
    b = {"i1": "116", "r1": "7", "p1": "hey"}
    rep = build_report(a, b, "v5", "v6")
    ids = [r["id"] for r in rep["regressions"]]
    assert "i1" in ids, ids                      # A passed, B failed -> regression
    assert all(r["id"] != "r1" for r in rep["regressions"])
    rep2 = build_report(b, a, "v5", "v6")
    assert [r["id"] for r in rep2["fixes"]] == ["i1"]
    print("selftest OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--a", help="baseline adapter path (the SERVED one, e.g. v5)")
    ap.add_argument("--b", help="candidate adapter path (e.g. v6)")
    ap.add_argument("--label-a", default="v5")
    ap.add_argument("--label-b", default="v6")
    ap.add_argument("--base", default=DEFAULT_BASE)
    ap.add_argument("--max-tokens", type=int, default=96)
    ap.add_argument("--out", help="write full JSON results here")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        selftest()
        return
    if not a.a or not a.b:
        ap.error("--a and --b are required (or --selftest)")
    for p in (a.a, a.b):
        if not os.path.isdir(p):
            sys.exit(f"adapter path not found: {p}")

    res_a = run_adapter(a.base, a.a, a.label_a, a.max_tokens, a.quiet)
    res_b = run_adapter(a.base, a.b, a.label_b, a.max_tokens, a.quiet)
    rep = build_report(res_a, res_b, a.label_a, a.label_b)
    clean = print_report(rep, a.label_a, a.label_b)

    if a.out:
        with open(a.out, "w") as f:
            json.dump({"base": a.base, "adapter_a": a.a, "adapter_b": a.b,
                       "report": rep}, f, indent=1)
        print(f"wrote {a.out}")
    sys.exit(0 if clean else 1)


if __name__ == "__main__":
    main()
