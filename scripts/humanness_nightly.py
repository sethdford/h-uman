#!/usr/bin/env python3
"""humanness_nightly.py — end-to-end nightly run of the humanness composite.

Part of the humanness north-star metric
(docs/plans/2026-05-29-humanness-north-star-metric/, Phase 4 / T10-T11).

Pipeline:
  1. read relationship-tagged fixtures (prompt, channel, contact_id, target_register)
  2. generate a reply per prompt against the local MLX server (HTTP, stdlib)
  3. `human eval score` -> per-axis JSON (A1 fidelity / A2 anti-AI / A4 relationship)
  4. humanness_compose -> composite + hybrid-gate verdict, append trailing baseline

The A3 Gemini judge is optional and not invoked here (graceful-degrade, AC-9):
its weight redistributes across the available axes. This orchestrator is the
composite-metric sibling of eval_fidelity_nightly.py (the fidelity-delta gate);
both can run under the same launchd schedule.

Stdlib only. Designed to run against the already-running realtime server
(default http://127.0.0.1:8741), so a 14-prompt run is ~15-30s, not the
full pre/post fidelity eval.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import humanness_compose as hc  # noqa: E402


def generate(server: str, model: str, prompt: str, max_tokens: int, temperature: float,
             system_prompt: str | None = None) -> str:
    # SOTA roadmap #19 (2026-07-18): with a system prompt this measures the
    # PRODUCTION prompt path (hu_persona_build_prompt output via
    # `human persona show <name> <channel>`) instead of a bare user message.
    # The first composite run's 0.763 was a floor precisely because this was
    # missing (finding #1 of that run).
    messages = []
    if system_prompt:
        messages.append({"role": "system", "content": system_prompt})
    messages.append({"role": "user", "content": prompt})
    body = json.dumps({
        "model": model,
        "messages": messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
    }).encode()
    req = urllib.request.Request(server.rstrip("/") + "/v1/chat/completions",
                                 data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = json.loads(resp.read())
    return data["choices"][0]["message"]["content"].strip()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Nightly humanness composite run.")
    ap.add_argument("--fixtures", default=str(
        HERE.parent / "docs/plans/2026-05-29-humanness-north-star-metric/data/"
        "relationship-prompts.jsonl"))
    ap.add_argument("--server", default="http://127.0.0.1:8741")
    ap.add_argument("--model", default="gemma-4-31b-it-4bit")
    ap.add_argument("--max-tokens", type=int, default=80)
    ap.add_argument("--temperature", type=float, default=0.7)
    ap.add_argument("--human-bin", default=str(HERE.parent / "build/human"))
    ap.add_argument("--target-style",
                    help="inline JSON communication-style for the A1 fidelity axis")
    ap.add_argument("--baseline", default=str(Path("~/.human/logs/humanness-baseline.jsonl")))
    ap.add_argument("--out", help="verdict JSON output path")
    ap.add_argument("--replies-out", help="dump generated replies JSONL here")
    ap.add_argument("--timestamp", help="ISO timestamp to stamp the baseline row")
    ap.add_argument("--no-append", action="store_true")
    ap.add_argument("--system-prompt-file",
                    help="file whose contents become the system message — dump the "
                         "production prompt via `human persona show <name> <channel>` "
                         "to measure the real prompt path (roadmap #19)")
    args = ap.parse_args(argv)

    system_prompt = None
    if args.system_prompt_file:
        system_prompt = Path(args.system_prompt_file).expanduser().read_text().strip()
        if not system_prompt:
            print("ERROR: --system-prompt-file is empty", file=sys.stderr)
            return 2
        print(f"  [prod-prompt] system message: {len(system_prompt)} chars "
              f"from {args.system_prompt_file}")

    fixtures = [json.loads(l) for l in Path(args.fixtures).expanduser().read_text().splitlines()
                if l.strip()]

    # 1+2. generate replies
    replies = []
    for fx in fixtures:
        try:
            reply = generate(args.server, args.model, fx["prompt"],
                             args.max_tokens, args.temperature,
                             system_prompt=system_prompt)
        except Exception as e:  # noqa: BLE001 — one bad prompt shouldn't kill the run
            print(f"  [warn] generation failed for {fx['prompt']!r}: {e}", file=sys.stderr)
            continue
        row = {"prompt": fx["prompt"], "reply": reply, "channel": fx.get("channel", "imessage")}
        if "contact_id" in fx:
            row["contact_id"] = fx["contact_id"]
        if "target_register" in fx:
            row["target_register"] = fx["target_register"]
        replies.append(row)
        print(f"  {fx.get('contact_id','?'):14s} | {reply[:60]}")

    if not replies:
        print("ERROR: no replies generated (is the MLX server up?)", file=sys.stderr)
        return 2

    replies_jsonl = "\n".join(json.dumps(r) for r in replies) + "\n"
    if args.replies_out:
        Path(args.replies_out).expanduser().write_text(replies_jsonl)

    # 3. score via the C CLI (ground-truth scorers)
    cmd = [args.human_bin, "eval", "score", "--in", "/dev/stdin"]
    if args.target_style:
        cmd += ["--target-style", args.target_style]
    proc = subprocess.run(cmd, input=replies_jsonl, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"ERROR: `human eval score` failed: {proc.stderr}", file=sys.stderr)
        return 2
    # the CLI prints the axes JSON on the last stdout line
    axes_doc = json.loads(proc.stdout.strip().splitlines()[-1])
    axes = axes_doc.get("axes", {})

    # 4. compose + gate
    baseline = hc.load_baseline(args.baseline, hc.DEFAULT_BASELINE_WINDOW)
    verdict = hc.compose(axes, baseline)
    verdict["n"] = axes_doc.get("n", len(replies))
    if args.baseline and not args.no_append:
        hc.append_baseline(args.baseline, axes, verdict["composite"], args.timestamp)

    out = json.dumps(verdict, indent=2)
    if args.out:
        Path(args.out).expanduser().parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).expanduser().write_text(out + "\n")
    print(out)
    print(f"\nHUMANNESS VERDICT: {verdict['humanness_verdict']}  composite={verdict['composite']:.3f}")
    return 1 if verdict["humanness_verdict"] == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
