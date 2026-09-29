#!/usr/bin/env python3
"""build_rule_preference_sheet.py — the human-judged round for the measured rules.

Rules 14 (emotional register) and 15 (substantive register) sit at SHADOW
behind a synthetic-judge A/B. The gate ladder's LIVE step needs a human
choice, and the person whose voice this is can make it in one sitting:

    real inbound (from chat.db)  ->  reply with the rule OFF  vs  reply with it LIVE
    "Which of these two would you rather have sent?"

Rows are the persona's real distress inbounds (rule 14, `reply_pairs.is_distress`)
and real substantive inbounds (rule 15, `reply_pairs.is_substantive`), each
answered once per arm by the serving model on :8741 with the product prompt
(`human persona show <persona> <channel>`, rules appended, gates from env).
Rows whose two replies are identical are skipped and counted: there is
nothing to prefer. Emits, into --out-dir:

  rating_sheet.csv   id, context, option_A, option_B, choice, confidence, axis_*
  answer_key.json    {"_mode": "preference", id: side holding the LIVE reply}
  arms.json          private provenance: rule, prompt bytes, both replies per row
  README.md          the question, the scoring command, what the number means

Scored by scripts/blind_ab/score_preference.py unchanged: its "win rate" is
here the share of rows where the LIVE arm was preferred — 0.5 is a coin flip,
above it the rule helped, below it the rule hurt. Both are recorded results.

Refuses (exit 3, writes nothing) when a live prompt renders identical to off
(gate did not flip / no card), when fewer than --min-rows survive, or when
:8741 is unreachable (exit 2). No message text leaves the machine.
"""
import argparse
import csv
import datetime
import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
from reply_pairs import fetch_reply_pairs, is_distress, is_substantive  # noqa: E402
from eval_distress_register import (  # noqa: E402
    DEFAULT_CHATDB,
    DEFAULT_MLX_URL,
    JudgeUnavailable,
    LocalJudge,
    MeasurementRefused,
    generate,
)
from make_rating_sheet import redact  # noqa: E402

SHEET_COLUMNS = ["id", "context", "option_A", "option_B", "choice", "confidence",
                 "axis_opinion", "axis_memory", "axis_reasoning", "axis_lexical",
                 "axis_tone", "axis_syntax"]
RULES = {
    "r14": {"gate": "HU_EMOTION_REGISTER", "predicate": is_distress, "label": "rule 14 emotional register"},
    "r15": {"gate": "HU_SUBSTANTIVE_REGISTER", "predicate": is_substantive, "label": "rule 15 substantive register"},
}


def render_prompt(human_bin, persona, channel, env_overrides):
    env = dict(os.environ, HU_EMOTION_REGISTER="off", HU_SUBSTANTIVE_REGISTER="off")
    env.update(env_overrides)
    r = subprocess.run([human_bin, "persona", "show", persona, channel], env=env,
                       capture_output=True, text=True, timeout=60)
    if r.returncode != 0 or not r.stdout.strip():
        raise MeasurementRefused(f"`{human_bin} persona show {persona} {channel}` failed "
                                 f"(rc={r.returncode}): {r.stderr.strip()[:200]}")
    return r.stdout


def build_rows(items, seed=42):
    """items: [{id, rule, context, off, live}] -> (rows, key, skipped_identical).
    Pure. Randomises which side holds the LIVE reply per row; the key records it."""
    rng = random.Random(seed)
    rows, key, skipped = [], {"_mode": "preference"}, 0
    for it in items:
        off, live = (it["off"] or "").strip(), (it["live"] or "").strip()
        if not off or not live or off == live:
            skipped += 1
            continue
        live_side = rng.choice("AB")
        a, b = (live, off) if live_side == "A" else (off, live)
        row = {c: "" for c in SHEET_COLUMNS}
        row.update({"id": it["id"], "context": redact(it["context"]),
                    "option_A": redact(a), "option_B": redact(b)})
        rows.append(row)
        key[it["id"]] = live_side
    return rows, key, skipped


def write_outputs(out_dir, rows, key, arms, skipped, meta):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "rating_sheet.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=SHEET_COLUMNS)
        w.writeheader()
        w.writerows(rows)
    with open(os.path.join(out_dir, "answer_key.json"), "w") as f:
        json.dump(key, f, indent=2)
    with open(os.path.join(out_dir, "arms.json"), "w", encoding="utf-8") as f:
        json.dump({"meta": meta, "skipped_identical": skipped, "rows": arms}, f, indent=2,
                  ensure_ascii=False)
    n14 = sum(1 for r in rows if r["id"].startswith("r14"))
    n15 = sum(1 for r in rows if r["id"].startswith("r15"))
    readme = f"""# Measured-rule preference sheet — {meta['date']}

{len(rows)} rows ({n14} rule 14 emotional-register, {n15} rule 15 substantive-register;
{skipped} skipped because both arms replied identically). Each row: a real incoming
`context` from your own chat.db, then `option_A` / `option_B`. Both are h-uman's reply
({meta['model']} on :8741, product prompt): one with the rule OFF, one with it LIVE.
Order randomised per row. Phone numbers and contact names redacted.

> Which of these two would you rather have SENT — not which one is you,
> which one would you rather your friend received?

Fill `choice` (A or B) and `confidence` (1 = coin flip … 5 = certain). Leave `axis_*`
blank unless you want to note why. Under 20 rated rows the scorer refuses.

Do not open `answer_key.json` or `arms.json` until you are done.

Score (from the repo root):
```
python3 scripts/blind_ab/score_preference.py {out_dir}/rating_sheet.csv \\
  --key {out_dir}/answer_key.json --rater human
```
The scorer's "win rate" is, for THIS sheet, the share of rows where you preferred the
LIVE arm. 0.50 is a coin flip; above it the rule helped; below it the rule hurt. Both
are real results. Score the two rules separately by filtering ids `r14-*` / `r15-*`.
"""
    with open(os.path.join(out_dir, "README.md"), "w") as f:
        f.write(readme)


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--human-bin", default=None)
    p.add_argument("--persona", default="seth")
    p.add_argument("--channel", default="imessage")
    p.add_argument("--chatdb", default=DEFAULT_CHATDB)
    p.add_argument("--days", type=int, default=365)
    p.add_argument("--max-per-rule", type=int, default=40)
    p.add_argument("--min-rows", type=int, default=20)
    p.add_argument("--temperature", type=float, default=0.7)
    p.add_argument("--max-tokens", type=int, default=80)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--mlx-url", default=os.environ.get("HUMAN_MLX_URL", DEFAULT_MLX_URL))
    p.add_argument("--out-dir", default=os.path.expanduser(
        "~/.human/blind_ab_preference/rules-" + datetime.date.today().isoformat()))
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    from eval_distress_register import default_human_bin
    human_bin = args.human_bin or default_human_bin()
    try:
        off_prompt = render_prompt(human_bin, args.persona, args.channel, {})
        live_prompts = {r: render_prompt(human_bin, args.persona, args.channel, {RULES[r]["gate"]: "live"})
                        for r in RULES}
    except MeasurementRefused as e:
        sys.stderr.write(f"REFUSED: {e}; wrote nothing.\n")
        return 3
    for r, lp in live_prompts.items():
        if lp == off_prompt:
            sys.stderr.write(f"REFUSED: {RULES[r]['gate']}=live rendered the same prompt as off; wrote nothing.\n")
            return 3
    try:
        model = LocalJudge(args.mlx_url).model()
    except JudgeUnavailable as e:
        sys.stderr.write(f"DEFERRED: :8741 unreachable ({e}); wrote nothing.\n")
        return 2
    rng = random.Random(args.seed)
    items, arms = [], []
    try:
        for r, spec in RULES.items():
            pairs = fetch_reply_pairs(args.chatdb, args.days, spec["predicate"])
            inbounds = sorted({i for i, _ in pairs})
            rng.shuffle(inbounds)
            inbounds = inbounds[:args.max_per_rule]
            sys.stderr.write(f"{spec['label']}: {len(inbounds)} inbounds x 2 arms on {model}\n")
            for k, inbound in enumerate(inbounds, start=1):
                off = generate(args.mlx_url, model, off_prompt, inbound, args.temperature, args.max_tokens)
                live = generate(args.mlx_url, model, live_prompts[r], inbound, args.temperature, args.max_tokens)
                rid = f"{r}-{k:02d}"
                items.append({"id": rid, "rule": r, "context": inbound, "off": off, "live": live})
                arms.append({"id": rid, "rule": spec["label"], "context": inbound, "off": off, "live": live})
    except JudgeUnavailable as e:
        sys.stderr.write(f"DEFERRED: {e}; wrote nothing.\n")
        return 2
    rows, key, skipped = build_rows(items, seed=args.seed)
    if len(rows) < args.min_rows:
        sys.stderr.write(f"REFUSED: only {len(rows)} rows differ between arms (< {args.min_rows}); wrote nothing.\n")
        return 3
    meta = {"date": datetime.date.today().isoformat(), "model": model, "human_bin": human_bin,
            "prompt_bytes": {"off": len(off_prompt), **{r: len(p) for r, p in live_prompts.items()}},
            "temperature": args.temperature, "max_tokens": args.max_tokens, "days": args.days}
    write_outputs(args.out_dir, rows, key, arms, skipped, meta)
    print(f"wrote {len(rows)} rows ({skipped} identical skipped) -> {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
