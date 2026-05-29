#!/usr/bin/env python3
"""Train seth-lora-v5-replyfirst via mlx_lm lora. scale=2.0 enforced.

Hyperparameters match the v4-repair recipe (lora-scale-default-or-die.md): rank=8,
scale=2.0 EXPLICIT, dropout=0.0, lr=1e-5, batch=1, num_layers=8, max_seq=2048.
Post-train: read adapter_config.json and HARD-FAIL if scale != 2.0.
Run on Apple Silicon: python3 scripts/train_replyfirst.py --iters 500
"""
import argparse
import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

MODEL_ID = "mlx-community/gemma-4-31b-it-4bit"
DATA_DIR = Path.home() / ".human/training-data"
ADAPTERS_DIR = DATA_DIR / "adapters"
LINEAGE = DATA_DIR / "adapter_lineage.jsonl"


def assert_scale_2(adapter_config: dict) -> None:
    """HARD-FAIL unless lora scale is exactly 2.0 (mlx_lm 0.31.2 default is 20.0)."""
    scale = adapter_config.get("lora_parameters", {}).get("scale")
    if scale != 2.0:
        raise SystemExit(
            f"FATAL: adapter scale={scale!r}, expected 2.0 "
            f"(lora-scale-default-or-die.md). Refusing to ship.")


def write_lora_config(cfg_path: Path, data_dir: Path, adapter_dir: Path,
                      iters: int) -> None:
    """mlx_lm reads scale from a YAML config; write it explicitly."""
    cfg = f"""model: "{MODEL_ID}"
train: true
data: "{data_dir}"
adapter_path: "{adapter_dir}"
iters: {iters}
batch_size: 1
num_layers: 8
learning_rate: 1.0e-5
max_seq_length: 2048
lora_parameters:
  rank: 8
  scale: 2.0
  dropout: 0.0
"""
    cfg_path.write_text(cfg)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iters", type=int, default=500)
    args = ap.parse_args()

    runid = datetime.now().strftime("%Y%m%d-%H%M%S")
    adapter_dir = ADAPTERS_DIR / f"seth-lora-v5-replyfirst-{runid}"
    adapter_dir.mkdir(parents=True, exist_ok=True)

    # mlx_lm lora expects train.jsonl/valid.jsonl in a data dir; symlink our splits.
    data_dir = DATA_DIR / f"replyfirst-data-{runid}"
    data_dir.mkdir(exist_ok=True)
    (data_dir / "train.jsonl").write_text((DATA_DIR / "replyfirst-train.jsonl").read_text())
    (data_dir / "valid.jsonl").write_text((DATA_DIR / "replyfirst-heldout.jsonl").read_text())

    cfg_path = adapter_dir / "lora_train_config.yaml"
    write_lora_config(cfg_path, data_dir, adapter_dir, args.iters)

    t0 = time.time()
    r = subprocess.run([sys.executable, "-m", "mlx_lm", "lora", "--config", str(cfg_path)])
    if r.returncode != 0:
        raise SystemExit(f"FATAL: mlx_lm lora exited {r.returncode}")

    # Post-train scale verification (lora-scale-default-or-die.md)
    ac_path = adapter_dir / "adapter_config.json"
    adapter_config = json.loads(ac_path.read_text())
    assert_scale_2(adapter_config)

    with LINEAGE.open("a") as f:
        f.write(json.dumps({
            "adapter": adapter_dir.name, "runid": runid, "base": MODEL_ID,
            "iters": args.iters, "scale": 2.0, "purpose": "reply-first ordering",
            "elapsed_sec": round(time.time() - t0, 1),
            "timestamp": datetime.now().isoformat(),
        }) + "\n")
    print(f"[train] DONE adapter={adapter_dir} scale=2.0 verified", flush=True)
    print(adapter_dir)  # last line = adapter path (consumed by the run task)
    return 0


if __name__ == "__main__":
    sys.exit(main())
