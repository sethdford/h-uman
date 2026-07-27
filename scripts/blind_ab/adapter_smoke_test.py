#!/usr/bin/env python3
"""Head-to-head smoke test of two LoRA adapters (same base, or cross-base).

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

    # cross-base: the served adapter is on a different base than the candidate
    python3 adapter_smoke_test.py \
        --a <glm_adapter> --base-a mlx-community/GLM-4.5-Air-4bit  --label-a glm-v5 \
        --b <gemma_adapter> --base-b mlx-community/gemma-4-31b-it-8bit --label-b v6

    python3 adapter_smoke_test.py --selftest      # checkers only, no model load
    python3 adapter_smoke_test.py --rescore old.json   # re-score, no GPU

A cross-base run answers "which served CONFIGURATION is better", not "which
adapter is better" — the delta carries the base model too. The script says so
on both stderr and stdout when the bases differ, because the numbers alone
invite the stronger claim.
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

def is_degenerate(text, max_cycle=12):
    """True if the reply collapses into a repeated token/phrase runaway.

    Two things this must get right, both learned from a real miss:

    1. **Cycle length is not bounded by 3.** v6 emitted "I'm not sure I'm a
       team" over and over — a SIX-word cycle — and an earlier version that
       only tested n-grams of length 1-3 scored that reply as clean. n now
       runs up to max_cycle (bounded by what the text can actually support).

    2. **Long cycles need fewer repetitions to be damning.** That same reply
       repeated its 6-gram only ~3 times, so a flat min_reps=4 would still
       have missed it. Short cycles need 4 reps (so "ha ha ha" stays clean),
       longer ones need 3 — 3x a 6-word phrase is 18 words of pure loop.
    """
    words = text.split()
    for n in range(1, min(max_cycle, len(words)) + 1):
        reps = 4 if n <= 2 else 3
        if len(words) < n * reps:
            break                      # no longer cycle can fit either
        for i in range(len(words) - n * reps + 1):
            gram = words[i:i + n]
            if all(words[i + k * n: i + (k + 1) * n] == gram for k in range(reps)):
                return True
    return False


ERROR_MARKERS = ("<error", "GENERATE_ERROR", "TIMEOUT", "[ERROR")


_SCAFFOLD_RE = re.compile(r"\A(?:\s*<think>.*?</think>\s*)+", re.S)


def strip_scaffolding(text):
    """Remove leading reasoning-channel scaffolding the server strips in prod.

    GLM emits a ``<think>…</think>`` block before the reply. mlx-server scrubs
    it before anything is sent, so the user never sees it — but this script
    calls mlx_lm directly and therefore does. Scoring the raw string punishes
    an adapter for a prefix production removes.

    That is not hypothetical: the first cross-base run (v6-gemma vs the SERVED
    glm-air-v5) scored glm-v5 **0 of 16** with 16 `leading_artifact` failures,
    every one of them this prefix. Read literally it said "production is
    totally broken, promote the candidate". Stripping first flips the result to
    12–10 in the SERVED adapter's favour. Any GLM adapter scored without this
    lands near zero, so the trap is silent and total.

    Blocks repeat: GLM emitted ``<think></think>\\n<think></think>\\n`` on 3 of
    16 prompts, and a single non-repeating substitution left the second one in
    place — which still tripped the artifact check.

    Only leading blocks are removed. A ``<think>`` appearing mid-reply is real
    output leaking scaffolding and should still fail.
    """
    return _SCAFFOLD_RE.sub("", text or "", count=1)


def evaluate(entry, text):
    """Per-response checks. Returns (passed, list_of_failure_tags)."""
    t = strip_scaffolding(text or "").strip()
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
        for lbl in (label_a, label_b):
            raw = r[lbl]["text"]
            shown = strip_scaffolding(raw)
            # Flag rather than hide: the voice call should be made on what the
            # user would receive, but a reader still needs to know the raw
            # output carried scaffolding.
            note = "  [<think> stripped]" if shown != raw else ""
            print(f"    {lbl}: {shown[:100]!r}{note}")

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
    # REGRESSION: the real v6 reply an earlier detector scored as clean — a
    # 6-word cycle repeated only ~3 times. Both the n<=3 bound and a flat
    # min_reps=4 would miss it.
    assert is_degenerate(
        "I'm not sure I'm a team I'm not sure I'm a team I'm not sure I'm a team I'm not sure I")
    assert is_degenerate("a b c d e f a b c d e f a b c d e f")      # 6-gram x3
    assert is_degenerate("one two three one two three one two three")  # 3-gram x3
    # ...without flagging ordinary terse texting (Seth's median reply is ~6 words)
    for ok in ("ok", "yeah", "7", "Same!", "no lol", "red, blue, yellow",
               "on my way", "hey what is up", "I'll send it over as soon as I'm out",
               "Yeah it's good. A bit of a learning curve with the legacy code."):
        assert not is_degenerate(ok), ok

    # REGRESSION: GLM reasoning-channel scaffolding must not be scored as
    # message text. The first cross-base run scored the SERVED glm-air-v5
    # 0/16 — every failure was this prefix, which mlx-server strips in prod.
    assert strip_scaffolding("<think></think>\nSure!") == "Sure!"
    assert strip_scaffolding("<think>reasoning here</think>\nyeah ok") == "yeah ok"
    # repeated blocks: GLM emitted two on 3 of 16 prompts, and a single
    # non-repeating substitution left the second one behind
    assert strip_scaffolding("<think></think>\n<think></think>\nYes") == "Yes"
    # a reply with no scaffolding must be untouched, byte for byte
    assert strip_scaffolding("on my way") == "on my way"
    assert strip_scaffolding("") == ""
    assert strip_scaffolding(None) == ""
    # mid-reply scaffolding is real leakage — do NOT rescue it
    assert "<think>" in strip_scaffolding("ok <think>hmm</think> sure")
    # end to end: the scaffolded reply must now score exactly like the bare one
    assert evaluate({}, "<think></think>\nSure!") == evaluate({}, "Sure!")
    assert not evaluate({"expect": r"\bred\b"}, "<think></think>\nred, green")[1]

    # --base-a/--base-b resolution: each side falls back to --base, and a run
    # is "cross" only when the two resolve differently. Checked without a model
    # load so the flag wiring cannot silently rot.
    def _resolve(base, ba, bb):
        a_, b_ = ba or base, bb or base
        return a_, b_, a_ != b_
    assert _resolve("X", None, None) == ("X", "X", False)      # same-base default
    assert _resolve("X", "Y", None) == ("Y", "X", True)        # side A overridden
    assert _resolve("X", None, "Y") == ("X", "Y", True)        # side B overridden
    assert _resolve("X", "Y", "Y") == ("Y", "Y", False)        # both, but equal
    assert _resolve("X", "Y", "Z") == ("Y", "Z", True)         # genuinely cross

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
    ap.add_argument("--base", default=DEFAULT_BASE,
                    help="base model for both sides (default: %(default)s)")
    ap.add_argument("--base-a", help="override the base for side A only — use when "
                    "the served adapter sits on a different base than the candidate")
    ap.add_argument("--base-b", help="override the base for side B only")
    ap.add_argument("--max-tokens", type=int, default=96)
    ap.add_argument("--out", help="write full JSON results here")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--rescore", help="re-evaluate a previous results JSON with the "
                    "CURRENT checkers (no model load, no GPU) — use after fixing a "
                    "checker so old runs get corrected verdicts")
    a = ap.parse_args()

    if a.selftest:
        selftest()
        return

    if a.rescore:
        with open(a.rescore) as f:
            prev = json.load(f)
        rows = prev["report"]["rows"]
        la, lb = a.label_a, a.label_b
        if rows and la not in rows[0]:
            sys.exit(f"--label-a/--label-b must match the saved file "
                     f"(found: {[k for k in rows[0] if k not in ('id','cat','prompt')]})")
        res_a = {r["id"]: r[la]["text"] for r in rows}
        res_b = {r["id"]: r[lb]["text"] for r in rows}
        rep = build_report(res_a, res_b, la, lb)
        clean = print_report(rep, la, lb)
        if a.out:
            with open(a.out, "w") as f:
                json.dump({**{k: v for k, v in prev.items() if k != "report"},
                           "report": rep, "rescored_from": a.rescore}, f, indent=1)
            print(f"wrote {a.out}")
        sys.exit(0 if clean else 1)
    if not a.a or not a.b:
        ap.error("--a and --b are required (or --selftest)")
    for p in (a.a, a.b):
        if not os.path.isdir(p):
            sys.exit(f"adapter path not found: {p}")

    base_a = a.base_a or a.base
    base_b = a.base_b or a.base
    cross = base_a != base_b
    if cross:
        print(f"CROSS-BASE: A={base_a}  B={base_b}\n"
              "  This measures BASE + ADAPTER as a unit. A difference is NOT\n"
              "  attributable to the adapter alone — read it as 'which served\n"
              "  configuration is better', not 'which adapter is better'.",
              file=sys.stderr)

    # Side A first: it is the SERVED config by convention, so if the second
    # load OOMs the run that matters most for a promotion call is captured.
    # One model resident at a time — run_adapter frees before returning.
    res_a = run_adapter(base_a, a.a, a.label_a, a.max_tokens, a.quiet)
    res_b = run_adapter(base_b, a.b, a.label_b, a.max_tokens, a.quiet)
    rep = build_report(res_a, res_b, a.label_a, a.label_b)
    clean = print_report(rep, a.label_a, a.label_b)
    if cross:
        print("NOTE: cross-base run — the delta includes the base model, not "
              "just the adapter.\n")

    if a.out:
        with open(a.out, "w") as f:
            json.dump({"base": a.base, "base_a": base_a, "base_b": base_b,
                       "cross_base": cross,
                       "adapter_a": a.a, "adapter_b": a.b,
                       "report": rep}, f, indent=1)
        print(f"wrote {a.out}")
    sys.exit(0 if clean else 1)


if __name__ == "__main__":
    main()
