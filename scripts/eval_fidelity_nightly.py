#!/usr/bin/env python3
"""
Nightly fidelity eval harness + SOTA gate for M3 continuous learning.

Loads 20-30 held-out iMessage prompts (real Seth conversation history,
date-stratified to avoid training contamination), runs two-pass inference
(base model alone, then base + persona-fidelity adapter), scores each
response using the deterministic shape classifier, computes bootstrap CI,
and gates on both statistical (one-sided t-test α=0.025) and practical
(delta ≥ 5%) significance.

Verdict logged to stdout and JSON, suitable for launchd scheduling.

Usage:
  # Preferred: no --adapter-path — the SERVING adapter is resolved dynamically
  # (live mlx-server process, else config.json personalization.lora_adapter_path)
  python3 scripts/eval_fidelity_nightly.py \\
    --model-id mlx-community/gemma-4-31b-it-4bit \\
    --output-json ~/.human/logs/eval-fidelity-nightly.json

  # Explicit override (pin a specific adapter under test)
  python3 scripts/eval_fidelity_nightly.py \\
    --adapter-path ~/.human/training-data/adapters/seth-lora-v5-8bit-20260718-105251

Exit codes:
  0 = PASS (gate executed, adapter measurably better)
  1 = FAIL (adapter measurably worse, or gate missing components)
  2 = DEFERRED (mlx_lm or model unavailable)
  3 = SKIP (no adapter / no prompts / no measurable delta) — deliberately
      non-zero and paired with a greppable FIDELITY_SKIP stdout marker so a
      silent skip can never masquerade as a healthy nightly again (a stale
      hardcoded adapter path skipped silently for 13 nights in 2026-07).
"""

import argparse
import json
import shlex
import statistics
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

# Reuse shared utilities
import sys
sys.path.insert(0, str(Path(__file__).parent))
from eval_fidelity_helpers import (
    bootstrap_ci,
    compute_persona_fidelity_scores,
    load_held_out_prompts_from_jsonl,
)  # noqa: E402
import adapter_registry

# Defaults
DEFAULT_FIXTURE = Path(__file__).parent.parent / "docs/plans/2026-05-26-sprint-56-gemma-as-seth/data/heldout-prompts.jsonl"
DEFAULT_LOG_DIR = Path.home() / ".human" / "logs"
DEFAULT_MODEL = "mlx-community/gemma-4-31b-it-4bit"
DEFAULT_CONFIG_PATH = Path.home() / ".human" / "config.json"

# Exit codes (see module docstring)
EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_DEFERRED = 2
EXIT_SKIP = 3

# SOTA gate thresholds (per design US-9, AC-9.5 and AC-9.6)
ALPHA_ONESIDED = 0.025  # one-sided t-test significance level
CONFIDENCE = 1 - 2 * ALPHA_ONESIDED  # 0.95 for two-sided, 0.975 for one-sided
PRACTICAL_DELTA_FLOOR = 0.05  # 5% absolute minimum improvement


def resolve_serving_adapter(
    ps_output: str | None = None,
    config_path: Path = DEFAULT_CONFIG_PATH,
) -> tuple[Path | None, str]:
    """Resolve the adapter that is ACTUALLY serving, not a hardcoded name.

    Priority (each candidate must exist on disk to win):
      1. The live mlx-server process's --adapter-path argument — ground truth
         for what is serving right now.
      2. config.json personalization.lora_adapter_path — what will serve
         after the next restart.

    Args:
        ps_output: process listing to scan (injectable for tests);
                   None = run `ps ax -o command` here
        config_path: path to ~/.human/config.json (injectable for tests)

    Returns:
        (adapter_path, source_description); (None, reason) when unresolvable.
    """
    if ps_output is None:
        try:
            ps_output = subprocess.run(
                ["ps", "ax", "-o", "command"],
                capture_output=True, text=True, timeout=10,
            ).stdout
        except Exception:
            ps_output = ""

    for line in ps_output.splitlines():
        if "mlx-server" not in line or "--adapter-path" not in line:
            continue
        try:
            tokens = shlex.split(line)
            candidate = Path(tokens[tokens.index("--adapter-path") + 1])
        except (ValueError, IndexError):
            continue
        if candidate.exists():
            return (candidate, "live mlx-server process --adapter-path")

    try:
        config = json.loads(Path(config_path).read_text())
        raw = config.get("personalization", {}).get("lora_adapter_path")
        if raw:
            candidate = Path(raw).expanduser()
            if candidate.exists():
                return (candidate, "config.json personalization.lora_adapter_path")
    except (OSError, json.JSONDecodeError):
        pass

    return (None, "no live mlx-server and no usable personalization.lora_adapter_path")


def emit_skip(reason: str, output_json: Path | None) -> int:
    """Log a SKIP loudly (greppable FIDELITY_SKIP marker), write verdict, exit 3."""
    verdict = {
        "timestamp": datetime.now().isoformat(),
        "verdict": "SKIP",
        "reason": reason,
        "exit_code": EXIT_SKIP,
    }
    print(f"[SKIP] FIDELITY_SKIP {reason}", flush=True)
    if output_json:
        output_json.write_text(json.dumps(verdict, indent=2))
    return EXIT_SKIP


def generate(model_id: str, prompt: str, adapter_path: str | None = None, max_tokens: int = 80) -> str:
    """Invoke mlx_lm.generate via subprocess.

    Args:
        model_id: HuggingFace model identifier
        prompt: input text
        adapter_path: optional LoRA adapter path
        max_tokens: max generation tokens (default 80 per design)

    Returns:
        Generated response string, or error marker if subprocess fails
    """
    cmd = [
        sys.executable, "-m", "mlx_lm", "generate",
        "--model", model_id,
        "--prompt", prompt,
        "--max-tokens", str(max_tokens),
        "--temp", "0.0",  # deterministic
    ]
    if adapter_path:
        cmd.extend(["--adapter-path", str(adapter_path)])

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        return "[timeout]"
    except Exception as e:
        return f"[gen_err: {str(e)[:100]}]"

    if result.returncode != 0:
        stderr_snippet = result.stderr[-150:].strip() if result.stderr else "(no stderr)"
        return f"[gen_err: {stderr_snippet}]"

    # mlx_lm.generate prints framed output with prompts, tokens/sec, etc.
    # Strip the metadata and keep only the actual response.
    out = result.stdout
    lines = [
        l for l in out.splitlines()
        if l and not l.startswith("==")
        and not l.startswith("Prompt")
        and not l.startswith("Generation:")
        and "tokens-per-sec" not in l
        and "Peak memory" not in l
    ]
    response = ("\n".join(lines)[-300:]).strip()
    return response if response else "[empty]"


def run_eval_pass(model_id: str, prompts: list[dict], adapter_path: str | None = None) -> tuple[list[str], dict]:
    """Run one pass of generation (pre or post adapter).

    Args:
        model_id: HuggingFace model
        prompts: list of prompt dicts with "prompt" field
        adapter_path: optional LoRA adapter path

    Returns:
        (responses, stats) where stats includes pass label and timing
    """
    start = time.time()
    responses = []
    pass_label = "POST (adapter)" if adapter_path else "PRE (base)"

    for i, p in enumerate(prompts):
        prompt_text = p["prompt"] if isinstance(p, dict) else p
        print(f"  [{pass_label}] {i+1}/{len(prompts)} {prompt_text[:50]!r}...", flush=True)

        response = generate(model_id, prompt_text, adapter_path=adapter_path)
        responses.append(response)

    elapsed = time.time() - start
    return (responses, {"pass": pass_label, "elapsed_sec": elapsed, "count": len(responses)})


def main():
    ap = argparse.ArgumentParser(
        description="Nightly fidelity eval harness + SOTA gate",
    )
    ap.add_argument(
        "--adapter-path",
        type=Path,
        default=None,
        help="Explicit LoRA adapter override. Default: resolve the SERVING "
             "adapter dynamically (live mlx-server process, then config.json "
             "personalization.lora_adapter_path).",
    )
    ap.add_argument(
        "--resolve-only",
        action="store_true",
        help="Print the resolved serving adapter path and exit (0 resolved, 3 not)",
    )
    ap.add_argument(
        "--min-prompts",
        type=int,
        default=20,
        help="Minimum held-out prompts required (default 20; lower only for "
             "manual small-n smoke runs — results below 20 are not gate-grade)",
    )
    ap.add_argument(
        "--model-id",
        default=DEFAULT_MODEL,
        help=f"HuggingFace model ID (default: {DEFAULT_MODEL})",
    )
    ap.add_argument(
        "--held-out-fixture",
        type=Path,
        default=DEFAULT_FIXTURE,
        help=f"Path to heldout-prompts.jsonl (default: {DEFAULT_FIXTURE})",
    )
    ap.add_argument(
        "--held-out-db-path",
        type=Path,
        help="Alternative: query prompts from this DB (not implemented; fixture takes precedence)",
    )
    ap.add_argument(
        "--output-json",
        type=Path,
        help="Write gate verdict JSON to this path (default: stdout only)",
    )
    ap.add_argument(
        "--log-dir",
        type=Path,
        default=DEFAULT_LOG_DIR,
        help=f"Directory for logs (default: {DEFAULT_LOG_DIR})",
    )
    args = ap.parse_args()

    # Ensure log dir exists before anything that might emit a verdict JSON
    args.log_dir.mkdir(parents=True, exist_ok=True)

    # Resolve the adapter under test: explicit override wins, else the one
    # actually serving. A hardcoded adapter name went stale in 2026-07 and
    # skipped silently for 13 nights — dynamic resolution is the default.
    if args.adapter_path is None:
        resolved, source = resolve_serving_adapter()
        if args.resolve_only:
            # stdout is a path-or-empty contract for shell callers; noise → stderr
            if resolved is None:
                print(f"[SKIP] FIDELITY_SKIP no serving adapter resolvable ({source})",
                      file=sys.stderr)
                return EXIT_SKIP
            print(resolved)
            return EXIT_PASS
        if resolved is None:
            return emit_skip(f"no serving adapter resolvable ({source})", args.output_json)
        print(f"[INFO] Resolved serving adapter via {source}: {resolved}", flush=True)
        args.adapter_path = resolved
    elif args.resolve_only:
        print(args.adapter_path)
        return EXIT_PASS

    # Load held-out prompts
    print(f"[INFO] Loading held-out prompts from {args.held_out_fixture}", flush=True)
    prompts = load_held_out_prompts_from_jsonl(str(args.held_out_fixture))

    if not prompts:
        return emit_skip(
            f"Held-out prompts unavailable or empty: {args.held_out_fixture}",
            args.output_json,
        )

    print(f"[INFO] Loaded {len(prompts)} held-out prompts (min {args.min_prompts} required)", flush=True)
    if len(prompts) < args.min_prompts:
        return emit_skip(
            f"Insufficient held-out prompts: {len(prompts)} < {args.min_prompts}",
            args.output_json,
        )

    # Verify adapter exists (explicit --adapter-path may point anywhere)
    if not args.adapter_path.exists():
        return emit_skip(f"Adapter not found: {args.adapter_path}", args.output_json)

    # PRE pass (base model only)
    print(f"\n=== PRE PASS (base model) ===", flush=True)
    try:
        pre_responses, pre_stats = run_eval_pass(args.model_id, prompts)
    except Exception as e:
        verdict = {
            "timestamp": datetime.now().isoformat(),
            "verdict": "DEFERRED",
            "reason": f"PRE pass failed: {str(e)[:200]}",
            "exit_code": 2,
        }
        print(f"[DEFERRED] {verdict['reason']}")
        if args.output_json:
            args.output_json.write_text(json.dumps(verdict, indent=2))
        return 2

    # POST pass (base + adapter)
    print(f"\n=== POST PASS (base + adapter) ===", flush=True)
    try:
        post_responses, post_stats = run_eval_pass(
            args.model_id, prompts, adapter_path=str(args.adapter_path)
        )
    except Exception as e:
        verdict = {
            "timestamp": datetime.now().isoformat(),
            "verdict": "DEFERRED",
            "reason": f"POST pass failed: {str(e)[:200]}",
            "exit_code": 2,
        }
        print(f"[DEFERRED] {verdict['reason']}")
        if args.output_json:
            args.output_json.write_text(json.dumps(verdict, indent=2))
        return 2

    # Score responses
    print(f"\n=== SCORING ===", flush=True)
    pre_classifications, pre_mean = compute_persona_fidelity_scores(pre_responses, channel="imessage")
    post_classifications, post_mean = compute_persona_fidelity_scores(post_responses, channel="imessage")

    print(f"PRE mean score:  {pre_mean:.3f}", flush=True)
    print(f"POST mean score: {post_mean:.3f}", flush=True)

    # Check for zero scores
    if pre_mean == 0.0 or post_mean == 0.0:
        verdict = {
            "timestamp": datetime.now().isoformat(),
            "verdict": "FAIL",
            "reason": f"Zero mean score: pre={pre_mean}, post={post_mean}. No valid responses.",
            "exit_code": 1,
        }
        print(f"[FAIL] {verdict['reason']}")
        if args.output_json:
            args.output_json.write_text(json.dumps(verdict, indent=2))
        return 1

    # Compute per-prompt deltas for bootstrap CI
    deltas = [post_classifications[i]["score"] - pre_classifications[i]["score"]
              for i in range(len(pre_classifications))]
    delta_mean = statistics.mean(deltas)

    # Bootstrap CI on deltas
    print(f"\n=== BOOTSTRAP CI (N=100 resamples) ===", flush=True)
    delta_mean_boot, delta_lo, delta_hi = bootstrap_ci(
        deltas, n_resamples=100, confidence=CONFIDENCE
    )
    stderr = (delta_hi - delta_lo) / (2 * 1.96)  # approximate stderr from CI width

    print(f"Delta mean:  {delta_mean:.3f}", flush=True)
    print(f"Delta CI:    [{delta_lo:.3f}, {delta_hi:.3f}]", flush=True)
    print(f"Stderr est:  {stderr:.4f}", flush=True)

    # SOTA gate logic
    # (AC-9.5) Statistical: post_mean > pre_mean + 1.96 * stderr
    # (AC-9.6) Practical: delta >= 0.05
    # PASS iff BOTH hold; else FAIL or SKIP
    print(f"\n=== SOTA GATE ===", flush=True)

    stat_threshold = pre_mean + 1.96 * stderr
    stat_pass = post_mean > stat_threshold
    prac_pass = delta_mean >= PRACTICAL_DELTA_FLOOR

    print(f"Statistical (α={ALPHA_ONESIDED}): post_mean ({post_mean:.3f}) > "
          f"pre_mean ({pre_mean:.3f}) + 1.96*stderr ({1.96*stderr:.4f}) = {stat_threshold:.3f}")
    print(f"  → {['FAIL', 'PASS'][stat_pass]}", flush=True)

    print(f"Practical (floor={PRACTICAL_DELTA_FLOOR}): delta_mean ({delta_mean:.3f}) >= {PRACTICAL_DELTA_FLOOR}")
    print(f"  → {['FAIL', 'PASS'][prac_pass]}", flush=True)

    if stat_pass and prac_pass:
        final_verdict = "PASS"
        exit_code = EXIT_PASS
    elif not prac_pass:
        # No measurable improvement over base — a real measurement, but not a
        # promotable one. Exit 3 (not 0) so a SKIP streak is visible to callers.
        final_verdict = "SKIP"
        exit_code = EXIT_SKIP
    else:
        final_verdict = "FAIL"
        exit_code = EXIT_FAIL

    # Detailed verdict JSON
    verdict = {
        "timestamp": datetime.now().isoformat(),
        "verdict": final_verdict,
        "exit_code": exit_code,
        "n_prompts": len(prompts),
        "model_id": args.model_id,
        "adapter_path": str(args.adapter_path),
        "pre": {
            "mean_score": round(pre_mean, 4),
            "elapsed_sec": pre_stats["elapsed_sec"],
        },
        "post": {
            "mean_score": round(post_mean, 4),
            "elapsed_sec": post_stats["elapsed_sec"],
        },
        "delta": {
            "mean": round(delta_mean, 4),
            "ci_lower": round(delta_lo, 4),
            "ci_upper": round(delta_hi, 4),
            "stderr_est": round(stderr, 4),
        },
        "gate": {
            "statistical_pass": stat_pass,
            "statistical_threshold": round(stat_threshold, 4),
            "statistical_alpha": ALPHA_ONESIDED,
            "practical_pass": prac_pass,
            "practical_floor": PRACTICAL_DELTA_FLOOR,
        },
    }

    print(f"\n=== FINAL VERDICT: {final_verdict} ===", flush=True)
    if final_verdict == "SKIP":
        print(f"[SKIP] FIDELITY_SKIP no measurable improvement "
              f"(delta_mean={delta_mean:.4f} < floor={PRACTICAL_DELTA_FLOOR})", flush=True)

    # Output verdict JSON
    if args.output_json:
        args.output_json.write_text(json.dumps(verdict, indent=2))
        print(f"[INFO] Verdict written to {args.output_json}", flush=True)

    # Also log to structured log file
    log_file = args.log_dir / f"eval-fidelity-{datetime.now().strftime('%Y-%m-%d')}.json"
    log_file.write_text(json.dumps(verdict, indent=2))
    print(f"[INFO] Log written to {log_file}", flush=True)

    # Record evaluation result to adapter registry. A SKIP records score=None:
    # 13 nightly SKIPs once landed as {"score": 1.0, "verdict": "SKIP"} and
    # read as perfect evals in the registry history. Full scores stay in the
    # verdict JSON above; the registry score is only meaningful for PASS/FAIL.
    try:
        adapter_name = Path(args.adapter_path).name
        adapter_registry.record_eval(
            adapter_id=adapter_name,
            eval_name="fidelity-nightly",
            score=post_mean if final_verdict != "SKIP" else None,
            verdict=final_verdict,
            timestamp=datetime.now().isoformat()
        )
        print(f"[INFO] Eval result recorded to adapter registry", flush=True)
    except Exception as e:
        print(f"[WARN] Failed to record eval to registry: {e}", file=sys.stderr, flush=True)

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
