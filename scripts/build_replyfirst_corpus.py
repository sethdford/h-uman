#!/usr/bin/env python3
"""Build the reply-first SFT corpus by self-distilling v4-repair OFFLINE.

For each m3 user prompt: generate with v4-repair (mlx_lm.generate, offline), split
into (deliberation, reply), reorder to reply-first, write SFT JSONL. Holds out a
disjoint split for ordering eval. Parse failures are logged, never emitted.

NEVER touches the :8741 prod server. Run on Apple Silicon.
Run: python3 scripts/build_replyfirst_corpus.py --limit 400
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import replyfirst_corpus_lib as lib

MODEL_ID = "mlx-community/gemma-4-31b-it-4bit"
ADAPTER = Path.home() / ".human/training-data/adapters/seth-lora-v4-repair-20260525-071921"
CORPUS_IN = Path.home() / ".human/training-data/m3-corpus.jsonl"
OUT_DIR = Path.home() / ".human/training-data"
SENTINEL = lib.DEFAULT_SENTINEL  # substitute Task-0's choice if different


def generate(prompt: str, max_tokens: int = 256) -> str:
    """Offline mlx_lm.generate. Copied from eval_fidelity_nightly.py:55-102."""
    cmd = [sys.executable, "-m", "mlx_lm", "generate", "--model", MODEL_ID,
           "--adapter-path", str(ADAPTER), "--prompt", prompt,
           "--max-tokens", str(max_tokens), "--temp", "0.0"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return "[timeout]"
    if r.returncode != 0:
        return "[gen_err]"
    lines = [l for l in r.stdout.splitlines()
             if l and not l.startswith("==") and not l.startswith("Prompt")
             and not l.startswith("Generation:") and "tokens-per-sec" not in l
             and "Peak memory" not in l]
    return "\n".join(lines).strip()


def load_user_prompts(limit: int) -> list[str]:
    prompts = []
    for line in CORPUS_IN.read_text().splitlines():
        rec = json.loads(line)
        if rec.get("role") == "user" and rec.get("content", "").strip():
            prompts.append(rec["content"].strip())
        if len(prompts) >= limit:
            break
    return prompts


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=400, help="max prompts to distill")
    ap.add_argument("--heldout-frac", type=float, default=0.15)
    ap.add_argument("--dry-run", action="store_true",
                    help="skip generation; emit nothing; just validate prompt loading")
    args = ap.parse_args()

    prompts = load_user_prompts(args.limit)
    print(f"[corpus] loaded {len(prompts)} user prompts", flush=True)
    if args.dry_run:
        print("[corpus] dry-run: skipping generation"); return 0

    examples, failures = [], []
    for i, p in enumerate(prompts):
        raw = generate(p)
        if raw.startswith("[") and raw.endswith("]"):  # [timeout]/[gen_err]
            failures.append({"prompt": p, "raw": raw}); continue
        target = lib.build_target(raw, sentinel=SENTINEL)
        if target is None:
            failures.append({"prompt": p, "raw": raw}); continue
        examples.append(lib.format_sft_example(p, target))
        if i % 20 == 0:
            print(f"[corpus] {i}/{len(prompts)} ok={len(examples)} fail={len(failures)}", flush=True)

    n_heldout = max(1, int(len(examples) * args.heldout_frac))
    heldout, train = examples[:n_heldout], examples[n_heldout:]

    (OUT_DIR / "replyfirst-train.jsonl").write_text(
        "\n".join(json.dumps(e) for e in train) + "\n")
    (OUT_DIR / "replyfirst-heldout.jsonl").write_text(
        "\n".join(json.dumps(e) for e in heldout) + "\n")
    (OUT_DIR / "replyfirst-parse-failures.jsonl").write_text(
        "\n".join(json.dumps(f) for f in failures) + ("\n" if failures else ""))

    print(f"[corpus] DONE train={len(train)} heldout={len(heldout)} "
          f"failures={len(failures)}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
