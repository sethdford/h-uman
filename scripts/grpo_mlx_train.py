#!/usr/bin/env python3
# scripts/grpo_mlx_train.py
#
# Phase 4 Task 8 (RL SOTA): thin shell around the mlx-lm-lora GRPO trainer.
"""
GRPO (Group Relative Policy Optimization, Shao et al. 2024 — DeepSeekMath
§4.1.2) MLX subprocess wrapper.

The C side (src/ml/grpo_mlx.c) writes JSONL with per-row schema:
    {"prompt": "..."}

ONLY the prompt is consumed by GRPO — rollouts are sampled from the live
policy via the in-trainer rollout loop, so chosen/rejected text is
deliberately omitted by the C-side writer. Each row contributes
`--n-rollouts` sampled completions which are then scored by the reward
source (synthetic token-counting fn or Phase 3 hu_reward_model_t loaded
from `--reward-model-path`) and used to compute group-relative advantages
for the PPO-clipped policy update with the optional KL penalty.

Symbol probe (per Phase 4 Task 0 step 2): the canonical mlx-lm-lora GRPO
entrypoint is

    from mlx_lm_lora.trainer.grpo_trainer import train_grpo

If that import fails but the `mlx_lm_lora.train` CLI module accepts
`--train-mode grpo`, we fall back to spawning that CLI as a child
subprocess. Both paths surface the same exit-code contract:

Exit codes:
    0  Success — adapter_model.safetensors exists at args.adapter_out.
    2  mlx_lm_lora package not installed (canonical import probe failed
       AND the CLI fallback also failed to import).
    3  Data path missing, or adapter_model.safetensors missing/empty
       after the run.
    other  Underlying CLI's non-zero exit (training/model load failure).

Test mode (HU_E2E_TEST_MODE=1): the C side's HU_IS_TEST shortcut sets
this env var on the popen. We write a 0-byte sentinel
adapter_model.safetensors and exit 0 immediately — the C-side wiring
(JSONL serialisation, stdout-parse loop, vtable population) gets
validated end-to-end without spawning real MLX (which would download
Gemma-3-4B-it Q4_K_M, ~2.5 GB on cold cache).
"""
import argparse
import importlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(
        description="MLX GRPO training wrapper around mlx_lm_lora.trainer.grpo_trainer."
    )
    ap.add_argument("--input", required=True,
                    help="JSONL file (one row per prompt; {\"prompt\": \"...\"}).")
    ap.add_argument("--adapter-out", required=True,
                    help="Output directory; final adapter_model.safetensors written here.")
    ap.add_argument("--backbone-path", default="mlx-community/gemma-3-4b-it-bf16",
                    help="HF model id or local path for the base/policy/ref model.")
    ap.add_argument("--n-rollouts", type=int, default=4,
                    help="Group size N — completions sampled per prompt. GRPO baseline "
                         "degenerates at N=1; trl default is 8, our ship default is 4 "
                         "(umbrella §5 D6 rationale).")
    ap.add_argument("--clip-eps", type=float, default=0.2,
                    help="PPO ratio clip ε (trl/grpo_config.py convention).")
    ap.add_argument("--kl-beta", type=float, default=0.04,
                    help="KL penalty coefficient β. DeepSeek R1 default; 0 disables KL.")
    ap.add_argument("--iters", type=int, default=50)
    ap.add_argument("--reward-fn", choices=("synthetic", "rm"), default="synthetic",
                    help="Reward source. 'synthetic' uses an in-script token-counting "
                         "stub; 'rm' loads a Phase 3 hu_reward_model_t checkpoint.")
    ap.add_argument("--reward-model-path", default=None,
                    help="When --reward-fn rm, path to the RM checkpoint directory.")
    args = ap.parse_args()

    # HU_E2E_TEST_MODE shortcut — the C-side HU_IS_TEST guard sets this so we
    # never touch real MLX from the unit-test path. Mirrors Phase 3 KTO's
    # dummy-adapter shortcut but moved to Python so the C side stays
    # backend-agnostic.
    if os.environ.get("HU_E2E_TEST_MODE") == "1":
        Path(args.adapter_out).mkdir(parents=True, exist_ok=True)
        sentinel = Path(args.adapter_out) / "adapter_model.safetensors"
        # 0-byte sentinel — exists() passes, stat.st_size==0 is the marker
        # tests assert on (test_grpo_mlx_dummy_adapter_in_test_mode).
        sentinel.touch()
        print(f"[grpo_mlx_train] HU_E2E_TEST_MODE=1 — wrote sentinel {sentinel}",
              flush=True)
        return 0

    # Canonical symbol probe — matches mlx_lm_lora_grpo_available() in
    # src/ml/rl_trainer.c and src/ml/grpo_mlx.c. A partial install where the
    # `mlx_lm_lora` top-level imports but the GRPO trainer submodule is
    # missing was Phase 2's deferred-failure root cause; doing this probe at
    # script entry surfaces the problem at exit-code 2 instead of
    # mid-training.
    try:
        importlib.import_module("mlx_lm_lora.trainer.grpo_trainer")
    except ImportError as e_canon:
        # Canonical symbol missing — try the CLI-module fallback. mlx-lm-lora's
        # `python -m mlx_lm_lora.train --train-mode grpo` is the stable public
        # contract that the package owners maintain across minor versions.
        try:
            importlib.import_module("mlx_lm_lora.train")
        except ImportError as e_cli:
            print(
                f"[grpo_mlx_train] ERROR: neither mlx_lm_lora.trainer.grpo_trainer "
                f"({e_canon}) nor mlx_lm_lora.train ({e_cli}) is importable",
                file=sys.stderr,
            )
            print(
                "[grpo_mlx_train] Install with: pip install mlx-lm-lora",
                file=sys.stderr,
            )
            return 2

    if args.reward_fn == "rm" and not args.reward_model_path:
        print("[grpo_mlx_train] ERROR: --reward-fn rm requires --reward-model-path",
              file=sys.stderr)
        return 3

    data_path = Path(args.input)
    if not data_path.exists():
        print(f"[grpo_mlx_train] ERROR: --input path does not exist: {data_path}",
              file=sys.stderr)
        return 3

    Path(args.adapter_out).mkdir(parents=True, exist_ok=True)

    print(
        f"[grpo_mlx_train] starting "
        f"(backbone={args.backbone_path}, iters={args.iters}, "
        f"n_rollouts={args.n_rollouts}, clip_eps={args.clip_eps}, "
        f"kl_beta={args.kl_beta}, reward_fn={args.reward_fn}, "
        f"input={data_path}, adapter_out={args.adapter_out})",
        flush=True,
    )

    # Delegate to the `mlx_lm_lora.train` CLI module (same stable-contract
    # rationale as scripts/dpo_mlx_train.py and scripts/kto_mlx_train.py —
    # internal trainer symbols rotate between minor releases, the CLI doesn't).
    # The canonical-symbol probe above is the up-front gate; this run is the
    # actual training.
    with tempfile.TemporaryDirectory(prefix="hu_grpo_mlx_data_") as tmp_dir:
        if data_path.is_dir():
            cli_data_arg = str(data_path)
        else:
            cli_data_arg = tmp_dir
            shutil.copy(str(data_path), str(Path(tmp_dir) / "train.jsonl"))

        cmd = [
            sys.executable, "-m", "mlx_lm_lora.train",
            "--train",
            "--train-mode", "grpo",
            "--model", args.backbone_path,
            "--data", cli_data_arg,
            "--adapter-path", args.adapter_out,
            "--iters", str(args.iters),
            "--batch-size", "1",
            "--grpo-group-size", str(args.n_rollouts),
            "--grpo-epsilon", str(args.clip_eps),
            "--grpo-beta", str(args.kl_beta),
        ]
        if args.reward_fn == "rm" and args.reward_model_path:
            cmd.extend(["--grpo-reward-model", args.reward_model_path])
        print(f"[grpo_mlx_train] invoking: {' '.join(cmd)}", flush=True)
        result = subprocess.run(cmd, check=False)
        if result.returncode != 0:
            print(
                f"[grpo_mlx_train] CLI exited non-zero ({result.returncode})",
                file=sys.stderr,
            )
            return result.returncode

    safetensors = Path(args.adapter_out) / "adapter_model.safetensors"
    if not safetensors.exists() or safetensors.stat().st_size == 0:
        print(
            f"[grpo_mlx_train] ERROR: expected output {safetensors} "
            f"missing or empty after CLI run",
            file=sys.stderr,
        )
        return 3
    print(
        f"[grpo_mlx_train] DONE — adapter at {safetensors} "
        f"({safetensors.stat().st_size} bytes)",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
