#!/usr/bin/env python3
"""eval_distress_register.py — paired OFF-vs-LIVE measurement of rule 14 on
the replies the twin gives to a sad or frustrated text.

Why: on 2026-09-12 a contact sent "😓" and the model produced "I'm sorry to
hear that. How can I help you with this…" and, on retry, "I understand this
is frustrating. How can I help you…". Seth's own replies to that kind of
inbound (chat.db) are short and never the support register. Rule 14
(HU_EMOTION_REGISTER, src/persona/emotion_card.c) now says so; this script
measures whether the rule actually moves the generated register, which is
what feature-gate-requires-measurement.md asks for before flipping it live.

Design — the product's own prompt, not a hand-written one:
  * inbound texts = REAL distress inbounds from chat.db
    (emotion_register.fetch_distress_pairs), Seth's real replies as the bar;
  * system prompt = `human persona show <persona> imessage` rendered twice,
    with HU_EMOTION_REGISTER=off and =live (the same builder the daemon uses,
    incl. the ABSOLUTE RULES block appended the way agent_turn does);
  * generation on :8741 (the serving model + adapter), k samples per inbound
    per arm at the same temperature; the two arms see the same inbounds;
  * metrics per arm, judge-free first: support-scaffold rate and median
    length (emotion_register.SUPPORT_SCAFFOLDS, the same regex the card
    uses); then the local judge's sympathy and neutral shares
    (emotion_register.LocalJudge, identity recorded).
Caveat stated in the verdict: this is the persona prompt + model, not the
whole daemon turn (no director, memory, or outbound pipeline).

Exit: 0 measured (JSON written), 2 judge/server down (nothing written),
3 refused: too few pairs, prompt render failed, or the two prompts are
identical (the gate did not flip — nothing to compare).
"""
import argparse
import datetime
import json
import os
import re
import statistics
import subprocess
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emotion_register import (  # noqa: E402
    DEFAULT_MLX_URL,
    SUPPORT_SCAFFOLDS,
    JudgeUnavailable,
    LocalJudge,
    MeasurementRefused,
    distress_stats,
    fetch_distress_pairs,
    label_texts,
)

SCHEMA = "distress-register-ab/v1"
DEFAULT_OUTPUT = os.path.expanduser("~/.human/logs/eval-distress-register-latest.json")
DEFAULT_CHATDB = os.path.expanduser("~/Library/Messages/chat.db")
_THINK = re.compile(r"<think>.*?</think>", re.S)


def render_prompt(human_bin: str, persona: str, channel: str, mode: str) -> str:
    env = dict(os.environ, HU_EMOTION_REGISTER=mode)
    r = subprocess.run([human_bin, "persona", "show", persona, channel], env=env,
                       capture_output=True, text=True, timeout=60)
    if r.returncode != 0 or not r.stdout.strip():
        raise MeasurementRefused(f"`{human_bin} persona show {persona} {channel}` failed "
                                 f"(rc={r.returncode}): {r.stderr.strip()[:200]}")
    return r.stdout


def generate(base_url: str, model: str, system: str, user: str, temperature: float,
             max_tokens: int, timeout: float = 120.0) -> str:
    body = json.dumps({
        "model": model,
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
        "max_tokens": max_tokens,
        "temperature": temperature,
    }).encode("utf-8")
    req = urllib.request.Request(base_url.rstrip("/") + "/chat/completions", data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (localhost)
            payload = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise JudgeUnavailable(f"generation failed: {e}") from e
    try:
        text = payload["choices"][0]["message"]["content"] or ""
    except (KeyError, IndexError, TypeError):
        text = ""
    return _THINK.sub("", text).strip()


def arm_metrics(replies, labels) -> dict:
    n = len(replies)
    valid = [lab for lab in labels if lab]
    out = {
        "n": n,
        "empty": sum(1 for r in replies if not r),
        "scaffold_rate": sum(1 for r in replies if SUPPORT_SCAFFOLDS.search(r)) / n if n else 0.0,
        "median_chars": int(statistics.median(len(r) for r in replies)) if n else 0,
        "judge_n": len(valid),
    }
    if valid:
        out["sympathy_share"] = sum(1 for e, _ in valid if e == "sympathy") / len(valid)
        out["neutral_share"] = sum(1 for e, _ in valid if e == "neutral") / len(valid)
        out["amusement_share"] = sum(1 for e, _ in valid if e == "amusement") / len(valid)
        out["mean_intensity"] = sum(i for _, i in valid) / len(valid)
    return out


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--human-bin", default=None,
                   help="human binary (default: build-prod/human, else build/human next to this repo)")
    p.add_argument("--persona", default="seth")
    p.add_argument("--channel", default="imessage")
    p.add_argument("--chatdb", default=DEFAULT_CHATDB)
    p.add_argument("--days", type=int, default=120)
    p.add_argument("--min-pairs", type=int, default=5)
    p.add_argument("--samples", type=int, default=4, help="generations per inbound per arm")
    p.add_argument("--temperature", type=float, default=0.7)
    p.add_argument("--max-tokens", type=int, default=80)
    p.add_argument("--mlx-url", default=os.environ.get("HUMAN_MLX_URL", DEFAULT_MLX_URL))
    p.add_argument("--output-json", default=DEFAULT_OUTPUT)
    p.add_argument("--dry-run", action="store_true")
    return p.parse_args(argv)


def default_human_bin():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for c in ("build-prod/human", "build/human"):
        path = os.path.join(here, c)
        if os.access(path, os.X_OK):
            return path
    return "human"


def run(args) -> int:
    human_bin = args.human_bin or default_human_bin()
    pairs = fetch_distress_pairs(args.chatdb, args.days)
    if len(pairs) < args.min_pairs:
        sys.stderr.write(f"REFUSED: only {len(pairs)} distress pairs in chat.db (< {args.min_pairs}); "
                         "wrote nothing.\n")
        return 3
    try:
        prompt_off = render_prompt(human_bin, args.persona, args.channel, "off")
        prompt_live = render_prompt(human_bin, args.persona, args.channel, "live")
    except MeasurementRefused as e:
        sys.stderr.write(f"REFUSED: {e}; wrote nothing.\n")
        return 3
    if prompt_off == prompt_live:
        sys.stderr.write("REFUSED: HU_EMOTION_REGISTER=live rendered the same prompt as off — "
                         "no emotion card for this persona, or the gate did not flip; wrote nothing.\n")
        return 3
    judge = LocalJudge(args.mlx_url)
    try:
        model = judge.model()
    except JudgeUnavailable as e:
        sys.stderr.write(f"DEFERRED: :8741 unreachable ({e}); wrote nothing.\n")
        return 2
    inbounds = [i for i, _ in pairs]
    sys.stderr.write(f"{len(pairs)} distress inbounds x {args.samples} samples x 2 arms on {model}\n")
    arms = {}
    try:
        for name, system in (("off", prompt_off), ("live", prompt_live)):
            replies = []
            for inbound in inbounds:
                for _ in range(args.samples):
                    replies.append(generate(args.mlx_url, model, system, inbound,
                                            args.temperature, args.max_tokens))
            labels = label_texts(judge, [r or "(empty)" for r in replies])
            arms[name] = arm_metrics(replies, labels)
            arms[name]["samples"] = [{"inbound": inbounds[i // args.samples], "reply": r}
                                     for i, r in enumerate(replies)][:200]
            sys.stderr.write(f"  arm {name}: scaffold {arms[name]['scaffold_rate']:.2f} "
                             f"median {arms[name]['median_chars']} chars\n")
    except JudgeUnavailable as e:
        sys.stderr.write(f"DEFERRED: {e}; wrote nothing.\n")
        return 2
    seth = distress_stats(pairs)
    verdict = {
        "schema": SCHEMA,
        "generated_at": datetime.datetime.now().replace(microsecond=0).isoformat(),
        "source": "scripts/eval_distress_register.py",
        "caveat": "persona prompt (`human persona show`, rules appended) + serving model on :8741; "
                  "not the full daemon turn (no director, memory, outbound pipeline)",
        "judge": {"id": judge.id(), "model": model, "url": args.mlx_url},
        "human_bin": human_bin,
        "prompt_bytes": {"off": len(prompt_off), "live": len(prompt_live)},
        "rule14": prompt_live[prompt_live.find("14. Emotional register"):][:900]
        if "14. Emotional register" in prompt_live else None,
        "settings": {"samples": args.samples, "temperature": args.temperature,
                     "max_tokens": args.max_tokens, "days": args.days},
        "seth": seth,
        "arms": arms,
        "deltas_live_minus_off": {
            k: arms["live"].get(k, 0.0) - arms["off"].get(k, 0.0)
            for k in ("scaffold_rate", "median_chars", "sympathy_share", "neutral_share",
                      "amusement_share", "mean_intensity")
        },
    }
    if args.dry_run:
        v = dict(verdict)
        v["arms"] = {k: {kk: vv for kk, vv in a.items() if kk != "samples"} for k, a in arms.items()}
        print(json.dumps(v, indent=2, ensure_ascii=False))
        return 0
    os.makedirs(os.path.dirname(args.output_json) or ".", exist_ok=True)
    tmp = f"{args.output_json}.tmp-{os.getpid()}"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(verdict, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, args.output_json)
    d = verdict["deltas_live_minus_off"]
    print(f"MEASURED pairs={len(pairs)} seth: scaffold {seth['scaffold_rate']:.2f} median "
          f"{seth['median_chars']} | off: scaffold {arms['off']['scaffold_rate']:.2f} median "
          f"{arms['off']['median_chars']} sympathy {arms['off'].get('sympathy_share', 0):.2f} | "
          f"live: scaffold {arms['live']['scaffold_rate']:.2f} median {arms['live']['median_chars']} "
          f"sympathy {arms['live'].get('sympathy_share', 0):.2f} | delta scaffold "
          f"{d['scaffold_rate']:+.2f} sympathy {d['sympathy_share']:+.2f} -> {args.output_json}")
    return 0


def main(argv=None) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
