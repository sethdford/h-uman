#!/usr/bin/env python3
# scripts/kto_mlx_train.py
#
# Phase 3 Task 7 (RL SOTA): thin shell around the mlx-lm-lora KTO CLI.
"""
Mirrors scripts/dpo_mlx_train.py for one-sided KTO preference signals.

The C side (src/ml/kto_mlx.c) writes JSONL with per-row schema:
    {"prompt": "...", "completion": "...", "label": true/false}

where label=true is desirable and label=false is undesirable.

Exit codes:
    0  Success — adapters.safetensors exists at args.adapter_path.
    2  mlx_lm_lora package not installed (import probe failed).
    3  Data path missing or adapters.safetensors missing/empty after run.
    other  CLI's own non-zero exit.
"""
import argparse
import importlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(
        description="MLX KTO training wrapper around the mlx_lm_lora.train CLI."
    )
    ap.add_argument("--model", required=True, help="HF model id")
    ap.add_argument(
        "--data",
        required=True,
        help="Path to JSONL file or directory with train.jsonl",
    )
    ap.add_argument("--adapter-path", required=True, help="Output adapter directory")
    ap.add_argument("--iters", type=int, default=100)
    ap.add_argument("--beta", type=float, default=0.1)
    ap.add_argument("--lambda-d", type=float, default=1.0, help="Desirable weight")
    ap.add_argument("--lambda-u", type=float, default=1.0, help="Undesirable weight")
    args = ap.parse_args()

    try:
        importlib.import_module("mlx_lm_lora.train")
    except ImportError as e:
        print(
            f"[kto_mlx_train] ERROR: mlx-lm-lora package not available: {e}",
            file=sys.stderr,
        )
        print(
            "[kto_mlx_train] Install with: pip install mlx-lm-lora",
            file=sys.stderr,
        )
        return 2

    data_path = Path(args.data)
    if not data_path.exists():
        print(
            f"[kto_mlx_train] ERROR: --data path does not exist: {data_path}",
            file=sys.stderr,
        )
        return 3

    Path(args.adapter_path).mkdir(parents=True, exist_ok=True)

    print(
        f"[kto_mlx_train] starting "
        f"(model={args.model}, iters={args.iters}, beta={args.beta}, "
        f"lambda_d={args.lambda_d}, lambda_u={args.lambda_u}, "
        f"data={data_path}, adapter_path={args.adapter_path})",
        flush=True,
    )

    with tempfile.TemporaryDirectory(prefix="hu_kto_mlx_data_") as tmp_dir:
        if data_path.is_dir():
            cli_data_arg = str(data_path)
        else:
            cli_data_arg = tmp_dir
            shutil.copy(str(data_path), str(Path(tmp_dir) / "train.jsonl"))

        cmd = [
            sys.executable,
            "-m",
            "mlx_lm_lora.train",
            "--train",
            "--train-mode",
            "kto",
            "--model",
            args.model,
            "--data",
            cli_data_arg,
            "--adapter-path",
            args.adapter_path,
            "--iters",
            str(args.iters),
            "--beta",
            str(args.beta),
            "--batch-size",
            "1",
        ]
        # Phase 3 audit fold-in (critic MEDIUM-5): forward --lambda-d/--lambda-u
        # to the upstream CLI. The C caller (src/ml/kto_mlx.c::kto_mlx_step)
        # passes configured weights through snprintf and expects them to take
        # effect — without these flags, mlx-lm-lora used its hard-coded defaults
        # (typically 1.0/1.0) regardless of what the user configured.
        # NOTE: Some mlx-lm-lora releases accept these flags via --kto-* prefix
        # or via the desirable_weight/undesirable_weight HF dataset columns.
        # If the upstream CLI rejects them, the popen call surfaces the error
        # to the caller (return code != 0); this is preferable to silently
        # ignoring user-configured hyperparameters.
        if args.lambda_d != 1.0:
            cmd.extend(["--desirable-weight", str(args.lambda_d)])
        if args.lambda_u != 1.0:
            cmd.extend(["--undesirable-weight", str(args.lambda_u)])
        print(f"[kto_mlx_train] invoking: {' '.join(cmd)}", flush=True)
        result = subprocess.run(cmd, check=False)
        if result.returncode != 0:
            print(
                f"[kto_mlx_train] CLI exited non-zero ({result.returncode})",
                file=sys.stderr,
            )
            return result.returncode

    safetensors = Path(args.adapter_path) / "adapters.safetensors"
    if not safetensors.exists() or safetensors.stat().st_size == 0:
        print(
            f"[kto_mlx_train] ERROR: expected output {safetensors} "
            f"missing or empty after CLI run",
            file=sys.stderr,
        )
        return 3
    print(
        f"[kto_mlx_train] DONE — adapter at {safetensors} "
        f"({safetensors.stat().st_size} bytes)",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
