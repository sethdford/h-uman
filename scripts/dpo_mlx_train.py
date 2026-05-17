#!/usr/bin/env python3
# scripts/dpo_mlx_train.py
#
# Phase 2 Task 7 (RL SOTA): thin shell around the mlx-lm-lora CLI.
"""
Why this script delegates to the CLI instead of importing internal APIs.

mlx-lm-lora's public, stable interface is the CLI exposed by
`python -m mlx_lm_lora.train`. Internal names that earlier drafts of this
wrapper called directly — `mlx_lm_lora.utils.PreferenceDataset`,
`mlx_lm_lora.trainer.dpo_trainer.train_dpo`, the `DPOTrainingArgs`
constructor — are private to the package and rotate between releases (e.g.
2.1.0 does not export `PreferenceDataset` from `mlx_lm_lora.utils` at all).

The CLI is the contract the package owners maintain across minor versions,
so this wrapper:
  - takes the same argparse surface the C side already invokes via popen
    (--model --data --adapter-path --iters --beta --batch-size)
  - translates `--data <file.jsonl>` (what src/ml/dpo_real_mlx.c writes)
    into `--data <dir>` (what mlx_lm_lora.train's load_local_dataset
    requires: a directory containing train.jsonl/valid.jsonl/test.jsonl)
  - propagates the CLI's exit code, plus our own contract:

Exit codes:
    0  Success — adapters.safetensors exists at args.adapter_path.
    2  mlx_lm_lora package not installed (import probe failed).
    3  Subprocess succeeded but adapters.safetensors missing/empty.
    other  CLI's own non-zero exit (training/model load failure).

Usage:
    dpo_mlx_train.py --model <hf_id> --data <jsonl_path_or_dir>
                     --adapter-path <dir>
                     --iters <N> --beta <beta> [--batch-size <B>]
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
        description="MLX DPO training wrapper around the mlx_lm_lora.train CLI."
    )
    ap.add_argument(
        "--model",
        required=True,
        help="HF model id, e.g. mlx-community/gemma-3-4b-it-bf16",
    )
    ap.add_argument(
        "--data",
        required=True,
        help=(
            "Path to a JSONL preference-pairs file (one row per pair; keys "
            "'chosen' and 'rejected' required, 'prompt' optional) OR an "
            "existing directory containing train.jsonl/valid.jsonl/test.jsonl."
        ),
    )
    ap.add_argument(
        "--adapter-path",
        required=True,
        help="Output directory; mlx_lm_lora writes adapters.safetensors here.",
    )
    ap.add_argument("--iters", type=int, default=100)
    ap.add_argument("--beta", type=float, default=0.1)
    ap.add_argument("--batch-size", type=int, default=1)
    args = ap.parse_args()

    # Probe import. We don't touch any internal symbol — just verify the
    # package is on sys.path before spawning the CLI subprocess.
    try:
        importlib.import_module("mlx_lm_lora.train")
    except ImportError as e:
        print(
            f"[dpo_mlx_train] ERROR: mlx-lm-lora package not available: {e}",
            file=sys.stderr,
        )
        print(
            "[dpo_mlx_train] Install with: pip install mlx-lm-lora",
            file=sys.stderr,
        )
        return 2

    data_path = Path(args.data)
    if not data_path.exists():
        print(
            f"[dpo_mlx_train] ERROR: --data path does not exist: {data_path}",
            file=sys.stderr,
        )
        return 3

    Path(args.adapter_path).mkdir(parents=True, exist_ok=True)

    print(
        f"[dpo_mlx_train] starting "
        f"(model={args.model}, iters={args.iters}, beta={args.beta}, "
        f"batch_size={args.batch_size}, data={data_path}, "
        f"adapter_path={args.adapter_path})",
        flush=True,
    )

    with tempfile.TemporaryDirectory(prefix="hu_dpo_mlx_data_") as tmp_dir:
        # mlx_lm_lora.train's `--data` expects either an HF dataset id or a
        # directory containing {train,valid,test}.jsonl
        # (see venv/lib/.../mlx_lm_lora/trainer/datasets.py::load_local_dataset).
        # The C side writes a single JSONL file, so when args.data is a file
        # we materialise a one-file shim directory pointing at it as
        # train.jsonl. Missing valid.jsonl/test.jsonl is fine — load_subset()
        # returns [] and the DPO trainer skips eval when len(val_dataset)==0.
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
            "dpo",
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
            str(args.batch_size),
        ]
        print(f"[dpo_mlx_train] invoking: {' '.join(cmd)}", flush=True)
        result = subprocess.run(cmd, check=False)
        if result.returncode != 0:
            print(
                f"[dpo_mlx_train] CLI exited non-zero ({result.returncode})",
                file=sys.stderr,
            )
            return result.returncode

    safetensors = Path(args.adapter_path) / "adapters.safetensors"
    if not safetensors.exists() or safetensors.stat().st_size == 0:
        print(
            f"[dpo_mlx_train] ERROR: expected output {safetensors} "
            f"missing or empty after CLI run",
            file=sys.stderr,
        )
        return 3
    print(
        f"[dpo_mlx_train] DONE — adapter at {safetensors} "
        f"({safetensors.stat().st_size} bytes)",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
