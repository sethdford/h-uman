#!/usr/bin/env python3
"""
Fine-tune Gemma 4 models with LoRA using MLX.

Supports multiple model targets for different latency/quality tradeoffs:
  - 31B dense: highest quality, ~20 tok/s (cloud-assist or high-end hardware)
  - E4B edge: native audio, ~110 tok/s (real-time voice on Mac)
  - E2B edge: fastest, ~180 tok/s (speculative decode draft model)

Wraps mlx_lm.lora with defaults tuned for persona fine-tuning on Apple Silicon.
Stops the MLX server before training (they compete for memory), restarts after.

Usage:
  python3 scripts/finetune-gemma.py --data ~/.human/training-data/finetune
  python3 scripts/finetune-gemma.py --target e4b --data ~/.human/training-data/finetune
  python3 scripts/finetune-gemma.py --target e2b --data ~/.human/training-data/finetune
  python3 scripts/finetune-gemma.py --resume  # resume from last adapter checkpoint
  python3 scripts/finetune-gemma.py --target e4b --speculative-draft  # train draft model for spec decode

Prerequisites:
  pip install mlx-lm (already installed if mlx-server.py works)
  pip install "mlx-lm-lora>=2.1.0,<3"  # required for the DPO preference pass

DPO note:
  Stock `mlx_lm` does NOT support DPO (no `--fine-tune-type dpo`; PR #794 closed,
  PR #417 still open). The third-party fork `mlx-lm-lora` is a drop-in superset
  that adds DPO/ORPO/CPO/KTO/GRPO behind `python3 -m mlx_lm_lora.train`. We pin
  `mlx-lm-lora>=2.1.0,<3` and document the upgrade path: if upstream `mlx_lm`
  lands native DPO, migrate back to stock by swapping the module and flag spelling
  (`--train-mode dpo` -> `--fine-tune-type dpo`).
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path


# ── Sprint 11 / US-11.3 — chosen_r early-stopping ────────────────────
def _load_early_stop_module():
    """Lazy-load `scripts/dpo_early_stop.py` (sibling file).

    Same-directory import. Lazy so this script can still parse in
    environments where the early-stopping module is absent (pure stdlib
    today; the seam is here for forward-compat with Sprint 12 persona-
    vector hooks).
    """
    here = Path(__file__).resolve().parent
    es_path = here / "dpo_early_stop.py"
    spec = importlib.util.spec_from_file_location("dpo_early_stop", es_path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules.setdefault("dpo_early_stop", mod)
    spec.loader.exec_module(mod)
    return mod


# ── Model Target Registry ─────────────────────────────────────────
MODEL_TARGETS = {
    "31b": {
        "model": "mlx-community/gemma-4-31b-it-4bit",
        "description": "Dense 31B — highest quality, ~20 tok/s",
        "default_iters": 800,
        "default_batch_size": 1,
        "default_lr": 1e-6,
        "default_num_layers": 8,
        "default_max_seq_length": 2048,
        # Sprint 7 / US-7.4: bumped from 16 -> 32. DPO benefits from
        # higher-rank adapters that can represent the contrastive signal
        # more expressively. e4b/e2b stay at 32 (out of scope per story).
        "default_rank": 32,
        "adapter_suffix": "seth-lora",
    },
    "e4b": {
        "model": "mlx-community/gemma-4-e4b-it-4bit",
        "description": "Edge 4B — native audio, ~110 tok/s, real-time voice",
        "default_iters": 1200,
        "default_batch_size": 4,
        "default_lr": 2e-5,
        "default_num_layers": 12,
        "default_max_seq_length": 4096,
        "default_rank": 32,
        "adapter_suffix": "seth-lora-e4b",
    },
    "e2b": {
        "model": "mlx-community/gemma-4-e2b-it-4bit",
        "description": "Edge 2B — speculative decode draft, ~180 tok/s",
        "default_iters": 1500,
        "default_batch_size": 8,
        "default_lr": 5e-5,
        "default_num_layers": 16,
        "default_max_seq_length": 4096,
        "default_rank": 32,
        "adapter_suffix": "seth-lora-e2b",
    },
}

DEFAULT_TARGET = "31b"
DEFAULT_DATA = str(Path.home() / ".human" / "training-data" / "finetune")
MLX_SERVER_PORT = 8741

# Sprint 7 / US-7.4 — default LoRA target modules.
# QKVO (a small expansion from the previous auto-discovery default which
# was effectively QV on Gemma's last-N blocks). When the user does not
# pass --target-modules, this list is used to materialize the `keys`
# field of `--lora-parameters`. Q+K+V+O are the conventional attention
# projection names for Gemma/Llama/Qwen/Mistral; the wider set
# (gate_proj,up_proj,down_proj for MLP) is opt-in via CLI.
DEFAULT_TARGET_MODULES = ["q_proj", "k_proj", "v_proj", "o_proj"]


def _build_lora_parameters_json(args) -> str:
    """Build the single argv token after `--lora-parameters` for mlx-lm-lora.

    Per Sprint 7 / US-7.4 design §0: target-module selection is passed to
    mlx-lm-lora via the existing `--lora-parameters` JSON flag, not via a
    dedicated `--target-modules` flag (no such flag exists on the mlx-lm
    side). The JSON's `keys` field is what mlx-lm's `linear_to_lora_layers`
    uses to override its auto-discovery. Stock `mlx_lm.lora` honors the
    same JSON shape, so this helper is used by BOTH `run_sft()` (Phase 1,
    stock mlx_lm) and `run_dpo()` (Phase 2, mlx-lm-lora fork).

    The dict is serialised via `json.dumps` (NOT f-string concatenation)
    so `subprocess.run` receives the whole JSON as a SINGLE argv slot. No
    shell quoting needed — we never use `shell=True`.
    """
    target_modules = list(getattr(args, "target_modules", None) or DEFAULT_TARGET_MODULES)
    rank = int(getattr(args, "rank", None) or 32)
    payload = {
        "rank": rank,
        "dropout": 0.0,
        "scale": 10.0,
        "keys": target_modules,
    }
    return json.dumps(payload)


def find_mlx_server_pid() -> int | None:
    """Find the PID of the running MLX server."""
    try:
        result = subprocess.run(
            ["lsof", "-ti", f":{MLX_SERVER_PORT}"],
            capture_output=True, text=True, timeout=5,
        )
        if result.returncode == 0 and result.stdout.strip():
            pids = result.stdout.strip().split("\n")
            return int(pids[0])
    except Exception:
        pass
    return None


def stop_mlx_server() -> int | None:
    """Stop the MLX server to free GPU memory for training."""
    pid = find_mlx_server_pid()
    if pid:
        print(f"  Stopping MLX server (PID {pid}) to free memory for training...")
        os.kill(pid, signal.SIGTERM)
        time.sleep(3)
        try:
            os.kill(pid, 0)
            os.kill(pid, signal.SIGKILL)
            time.sleep(1)
        except ProcessLookupError:
            pass
        print("  MLX server stopped.")
        return pid
    print("  No MLX server running (port 8741 free).")
    return None


def start_mlx_server(model: str, adapter_path: str | None = None):
    """Restart the MLX server, optionally with a LoRA adapter."""
    cmd = [
        sys.executable,
        str(Path(__file__).parent / "mlx-server.py"),
        "--model", model,
        "--port", str(MLX_SERVER_PORT),
    ]
    if adapter_path and Path(adapter_path).exists():
        cmd.extend(["--adapter-path", adapter_path])

    print(f"\n  Restarting MLX server...")
    print(f"    Model: {model}")
    if adapter_path:
        print(f"    Adapter: {adapter_path}")

    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    time.sleep(5)

    if proc.poll() is None:
        print(f"  MLX server started (PID {proc.pid}) on port {MLX_SERVER_PORT}")
    else:
        print(f"  WARNING: MLX server exited with code {proc.returncode}")


def version_adapter(adapter_dir: Path, config: dict) -> Path:
    """Save a versioned copy of the adapter with metadata. Returns the versioned path."""
    base = adapter_dir.parent
    version = 1
    while (base / f"seth-lora-v{version}").exists():
        version += 1
    versioned = base / f"seth-lora-v{version}"
    shutil.copytree(adapter_dir, versioned)

    config["version"] = version
    config["versioned_path"] = str(versioned)
    with open(versioned / "train_config.json", "w") as f:
        json.dump(config, f, indent=2)

    current_link = base / "seth-lora-current"
    if current_link.is_symlink():
        current_link.unlink()
    elif current_link.exists():
        shutil.rmtree(current_link)
    current_link.symlink_to(versioned.name)

    print(f"  Versioned adapter: {versioned}")
    print(f"  Current symlink: {current_link} -> {versioned.name}")
    return versioned


def get_target_config(target_name: str) -> dict:
    """Get model configuration for a target (31b, e4b, e2b)."""
    return MODEL_TARGETS.get(target_name, MODEL_TARGETS[DEFAULT_TARGET])


def resolve_adapter_path(args) -> str:
    """Resolve adapter path from args or target defaults."""
    if args.adapter_path:
        return args.adapter_path
    target_cfg = get_target_config(args.target)
    return str(Path.home() / ".human" / "training-data" / "adapters" / target_cfg["adapter_suffix"])


def resolve_model(args) -> str:
    """Resolve model name from args or target defaults."""
    if args.model:
        return args.model
    return get_target_config(args.target)["model"]


def run_sft(args, data_dir: Path, adapter_dir: Path) -> int:
    """Run SFT LoRA fine-tuning. Returns exit code.

    Sprint 8 US-8.6 fix: route through `mlx_lm_lora.train --train-mode sft`
    so the `lora_parameters` block is honored. Both stock `mlx_lm.lora` and
    `mlx_lm_lora.train` reject `--lora-parameters` as a CLI flag — it must
    go through `-c <yaml>`. We write a temporary YAML config containing the
    `lora_parameters` block and pass it via `-c`.
    """
    adapter_dir.mkdir(parents=True, exist_ok=True)

    # US-8.6: write lora_parameters to a YAML config file.
    import json as _json
    lora_params = _json.loads(_build_lora_parameters_json(args))
    config_path = adapter_dir / "sft_train_config.yaml"
    with open(config_path, "w") as cf:
        cf.write("lora_parameters:\n")
        cf.write(f"  rank: {lora_params['rank']}\n")
        cf.write(f"  dropout: {lora_params['dropout']}\n")
        cf.write(f"  scale: {lora_params['scale']}\n")
        cf.write("  keys:\n")
        for k in lora_params["keys"]:
            cf.write(f"    - {k}\n")

    model = resolve_model(args)
    cmd = [
        sys.executable, "-m", "mlx_lm_lora.train",
        "--model", model,
        "--train",
        "--train-mode", "sft",
        # US-11.2: operator-selectable adapter type. Defaults to "dora"
        # (Sprint 11 baseline upgrade) but accepts "lora" for back-compat.
        # getattr with default keeps synthesized Namespaces (older callers,
        # tests that predate US-11.2) from crashing.
        "--train-type", getattr(args, "train_type", "dora"),
        "--data", str(data_dir),
        "--adapter-path", str(adapter_dir),
        "--iters", str(args.iters),
        "--batch-size", str(args.batch_size),
        "--learning-rate", str(args.learning_rate),
        "--num-layers", str(args.num_layers),
        "--steps-per-report", str(args.steps_per_report),
        "--steps-per-eval", str(args.steps_per_eval),
        "--save-every", str(args.save_every),
        "--max-seq-length", str(args.max_seq_length),
        "--grad-checkpoint",
        "--optimizer", "adamw",
        "-c", str(config_path),
    ]

    if args.mask_prompt:
        cmd.append("--mask-prompt")

    if args.resume and (adapter_dir / "adapters.safetensors").exists():
        cmd.extend(["--resume-adapter-file", str(adapter_dir / "adapters.safetensors")])
        print(f"  Resuming from {adapter_dir / 'adapters.safetensors'}\n")

    print(f"  Running SFT: {' '.join(cmd[:8])}...")
    est_time = "5-15 minutes" if args.target in ("e2b", "e4b") else "20-60 minutes"
    print(f"  (Estimated: {est_time} depending on data size)\n")

    t0 = time.time()
    result = subprocess.run(cmd, cwd=str(Path(__file__).parent.parent))
    elapsed = time.time() - t0

    if result.returncode == 0:
        print(f"  SFT complete! ({elapsed/60:.1f} minutes)")
        if (adapter_dir / "adapters.safetensors").exists():
            size_mb = (adapter_dir / "adapters.safetensors").stat().st_size / (1024 * 1024)
            print(f"  Adapter size: {size_mb:.1f} MB")
    else:
        print(f"  SFT FAILED (exit code {result.returncode}, {elapsed:.0f}s)")

    return result.returncode


def find_dpo_data(data_dir: Path, from_corrections: bool = False) -> Path | None:
    """Look for DPO pairs in standard locations.

    Search order (first match wins):
      1. If `from_corrections=True`: ~/.human/dpo/pairs.jsonl (LOCKED path —
         this is where the US-7.2 outbound-corrections miner writes its
         deduplicated, PII-redacted preference pairs).
      2. ~/.human/dpo/pairs.jsonl is also probed unconditionally so a
         miner-produced file is picked up even without the flag.
      3. <data_dir>/dpo/pairs.jsonl
      4. <data_dir>/dpo_pairs.jsonl
      5. <data_dir>.parent/dpo/pairs.jsonl
      6. Newest <repo>/convo-training*/dpo_pairs.db
    """
    repo_root = Path(__file__).resolve().parent.parent
    home_corrections = Path.home() / ".human" / "dpo" / "pairs.jsonl"

    candidates: list[Path] = []
    if from_corrections:
        # Cross-story lock with US-7.2: the miner writes here.
        candidates.append(home_corrections)
    candidates.extend([
        home_corrections,
        data_dir / "dpo" / "pairs.jsonl",
        data_dir / "dpo_pairs.jsonl",
        data_dir.parent / "dpo" / "pairs.jsonl",
    ])
    # Newest convo-training directory first (most recent DPO pairs win)
    for pattern in sorted(repo_root.glob("convo-training*/dpo_pairs.db"), reverse=True):
        candidates.append(pattern)

    searched: list[str] = []
    for c in candidates:
        searched.append(str(c))
        if c.exists():
            print(f"  DPO data found: {c}")
            return c
    if from_corrections:
        # Make the failure observable when the user explicitly asked for the
        # corrections path — they should know which paths we probed.
        print(f"  --from-corrections: no DPO file found. Searched:")
        for s in searched:
            print(f"    - {s}")
    return None


def prepare_dpo_from_db(db_path: Path, output_dir: Path) -> Path | None:
    """Convert DPO pairs from SQLite DB to JSONL format for mlx-lm-lora DPO.

    Emits `{prompt, chosen, rejected}` lines into train.jsonl and valid.jsonl
    inside `output_dir`. 90/10 split; if too few rows for a true split, the
    valid file mirrors the first row so mlx-lm-lora's data loader is happy.
    """
    import sqlite3
    try:
        conn = sqlite3.connect(str(db_path))
        rows = conn.execute("SELECT prompt, chosen, rejected, margin FROM dpo_pairs").fetchall()
        conn.close()
    except Exception as e:
        print(f"  Could not read DPO DB {db_path}: {e}")
        return None

    rows = [(p, c, r, m) for (p, c, r, m) in rows if p and c and r]
    if not rows:
        return None

    output_dir.mkdir(parents=True, exist_ok=True)
    train_path = output_dir / "train.jsonl"
    valid_path = output_dir / "valid.jsonl"

    import random
    if os.environ.get("HU_DPO_DETERMINISTIC") == "1":
        random.seed(0)
    random.shuffle(rows)
    split = max(1, int(len(rows) * 0.9))

    train_rows = rows[:split]
    valid_rows = rows[split:] if len(rows) > split else rows[:1]

    for path, data in [(train_path, train_rows), (valid_path, valid_rows)]:
        with open(path, "w") as f:
            for prompt, chosen, rejected, _margin in data:
                entry = {
                    "prompt": prompt,
                    "chosen": chosen,
                    "rejected": rejected,
                }
                f.write(json.dumps(entry) + "\n")

    print(f"  DPO data: {len(rows)} pairs ({len(train_rows)} train, {len(valid_rows)} valid)")
    return output_dir


def _prepare_dpo_from_jsonl(jsonl_path: Path, output_dir: Path) -> Path | None:
    """Validate and stage a `{prompt, chosen, rejected}` JSONL into output_dir.

    Required keys per line: prompt, chosen, rejected. Optional: system.
    Writes `train.jsonl` (and an empty-but-valid `valid.jsonl` if needed) so
    mlx-lm-lora's data loader is satisfied.
    """
    rows: list[dict] = []
    try:
        with open(jsonl_path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if not all(k in obj and obj[k] for k in ("prompt", "chosen", "rejected")):
                    continue
                rows.append(obj)
    except OSError as e:
        print(f"  Could not read DPO JSONL {jsonl_path}: {e}")
        return None

    if not rows:
        print(f"  DPO JSONL {jsonl_path} had no valid {{prompt, chosen, rejected}} rows.")
        return None

    output_dir.mkdir(parents=True, exist_ok=True)
    import random
    if os.environ.get("HU_DPO_DETERMINISTIC") == "1":
        random.seed(0)
    random.shuffle(rows)
    split = max(1, int(len(rows) * 0.9))

    train_rows = rows[:split]
    valid_rows = rows[split:] if len(rows) > split else rows[:1]

    for path, data in [(output_dir / "train.jsonl", train_rows),
                       (output_dir / "valid.jsonl", valid_rows)]:
        with open(path, "w") as f:
            for entry in data:
                f.write(json.dumps(entry) + "\n")

    print(f"  DPO data: {len(rows)} pairs ({len(train_rows)} train, {len(valid_rows)} valid)")
    return output_dir


def run_dpo(args, adapter_dir: Path) -> int:
    """Run a genuine DPO preference pass via mlx-lm-lora.

    Replaces the legacy SFT-on-chosen-only path. Stock `mlx_lm` does not
    support DPO (see module docstring). We shell out to:

        python3 -m mlx_lm_lora.train --train --train-mode dpo --train-type lora
            --reference-model-path <base-model> --resume-adapter-file <sft>
            --data <dir-with-{train,valid}.jsonl-of-{prompt,chosen,rejected}>
            --beta 0.1 --dpo-cpo-loss-type sigmoid ...

    Non-fatal if no preference data is found (AC-7.1.3): the SFT adapter
    remains valid; we print a warning and return 0.
    """
    data_dir = Path(args.data)
    from_corrections = bool(getattr(args, "from_corrections", False))

    dpo_source = find_dpo_data(data_dir, from_corrections=from_corrections)
    if dpo_source is None:
        # AC-7.1.3: clearly-labeled warning, non-fatal exit, SFT adapter retained.
        print(
            "  [dpo] no preference data found at any candidate path; "
            "skipping (SFT adapter retained)"
        )
        return 0

    dpo_data_dir = data_dir / "dpo_prepared"

    src = str(dpo_source)
    if src.endswith(".db"):
        # SQLite source: prepare_dpo_from_db emits the correct
        # {prompt, chosen, rejected} JSONL format with a 90/10 split.
        prepared = prepare_dpo_from_db(dpo_source, dpo_data_dir)
        if prepared is None:
            print("  [dpo] DPO DB had no valid rows; skipping (SFT adapter retained)")
            return 0
    elif src.endswith(".jsonl"):
        prepared = _prepare_dpo_from_jsonl(dpo_source, dpo_data_dir)
        if prepared is None:
            print("  [dpo] DPO JSONL had no valid rows; skipping (SFT adapter retained)")
            return 0
    else:
        print(f"  [dpo] unknown DPO format: {dpo_source}; skipping")
        return 0

    dpo_iters = max(50, args.iters // 4)
    dpo_lr = args.learning_rate * 0.1
    model = resolve_model(args)

    # mlx-lm-lora CLI. The flag spelling differs from stock mlx_lm:
    #   --train-mode dpo       (selects the DPO loss head)
    #   --train-type lora      (the fork's renamed --fine-tune-type)
    # --reference-model-path MUST be set explicitly to the base model.
    # If omitted, mlx-lm-lora defaults reference == policy after
    # --resume-adapter-file, which produces a near-zero gradient on
    # step 1 (design §5.3).
    cmd = [
        sys.executable, "-m", "mlx_lm_lora.train",
        "--model", model,
        "--train",
        "--train-mode", "dpo",
        # US-11.2: must match the train_type used by run_sft (above) or
        # the DPO pass will fail to load the SFT-produced adapter (shape
        # mismatch). args.train_type is set once in main() and threaded
        # through every Namespace; getattr default keeps older callers safe.
        "--train-type", getattr(args, "train_type", "dora"),
        "--data", str(dpo_data_dir),
        "--adapter-path", str(adapter_dir),
        "--resume-adapter-file", str(adapter_dir / "adapters.safetensors"),
        "--reference-model-path", model,
        "--beta", "0.1",
        "--dpo-cpo-loss-type", "sigmoid",
        "--iters", str(dpo_iters),
        "--batch-size", "1",
        "--learning-rate", str(dpo_lr),
        "--num-layers", str(args.num_layers),
        "--max-seq-length", str(args.max_seq_length),
        "--grad-checkpoint",
        # Sprint 8 US-8.6 fix: lora_parameters via -c <yaml>, not CLI flag.
        # Reuse the SFT yaml if it exists (same params); write a fresh one
        # if SFT was skipped.
        "-c", str(adapter_dir / "dpo_train_config.yaml"),
    ]
    # Write the DPO config YAML (same lora_parameters as SFT).
    import json as _json
    _lp = _json.loads(_build_lora_parameters_json(args))
    with open(adapter_dir / "dpo_train_config.yaml", "w") as cf:
        cf.write("lora_parameters:\n")
        cf.write(f"  rank: {_lp['rank']}\n")
        cf.write(f"  dropout: {_lp['dropout']}\n")
        cf.write(f"  scale: {_lp['scale']}\n")
        cf.write("  keys:\n")
        for k in _lp["keys"]:
            cf.write(f"    - {k}\n")

    if getattr(args, "mask_prompt", False):
        cmd.append("--mask-prompt")

    print(f"\n  Running DPO: {dpo_iters} iters, LR={dpo_lr}, beta=0.1 (sigmoid)")
    print(f"  (Real preference loss via mlx-lm-lora; both chosen and rejected are used)\n")

    # Sprint 11 / US-11.3 — chosen_r plateau-break early stopping.
    # Default 'none' (NOT 'chosen_r') when args has no `early_stopping_signal`
    # attribute. This preserves backward-compat with Sprint 7/8 tests and
    # internal Namespaces (run_train_all, run_speculative_draft_training)
    # that predate this flag. main() sets it explicitly to the CLI value
    # (default 'chosen_r' for new operator-driven runs).
    es_signal = getattr(args, "early_stopping_signal", "none")
    t0 = time.time()
    if es_signal == "chosen_r":
        es_mod = _load_early_stop_module()
        detector = es_mod.ChosenRPlateauDetector(
            window=5,
            drop_ratio=0.5,
            confirm_consecutive=2,
            save_every=int(getattr(args, "save_every", 20)),
            signal_mode="chosen_r",
        )
        sentinel = adapter_dir / ".early_stop"
        event_log = adapter_dir / "early_stop_events.jsonl"
        print(f"  Early stopping: chosen_r plateau-break "
              f"(window=5, drop_ratio=0.5, confirm=2, save_every={detector.save_every})")
        rc, decision = es_mod.run_with_early_stop(
            cmd=cmd,
            detector=detector,
            cwd=str(Path(__file__).parent.parent),
            sentinel_path=sentinel,
            event_log_path=event_log,
        )
        if decision is not None:
            print(
                f"  Early stop FIRED: first_breach_iter={decision.first_breach_iter}, "
                f"stopped_iter={decision.stopped_iter}, "
                f"promoted_iter={decision.promoted_iter}"
            )
            print(f"  Promoted adapter: iter {decision.promoted_iter} (last save_every boundary)")
        returncode = rc
    else:
        result = subprocess.run(cmd, cwd=str(Path(__file__).parent.parent))
        returncode = result.returncode
    elapsed = time.time() - t0

    if returncode == 0:
        print(f"  DPO complete! ({elapsed/60:.1f} minutes)")
    else:
        print(f"  DPO failed (exit code {returncode}, {elapsed:.0f}s)")
        print(f"  Primary SFT adapter is still valid — non-fatal.")

    return 0


def run_speculative_draft_training(args):
    """Train a draft model (E2B) that shadows the target model for speculative decoding.

    The draft model is fine-tuned on the SAME data as the target, so its token
    distribution closely matches. This maximizes the acceptance rate during
    speculative decoding (target accepts more draft proposals = faster inference).
    """
    print(f"\n{'='*60}")
    print(f"  Speculative Draft Model Training")
    print(f"{'='*60}")

    target_cfg = get_target_config(args.target)
    draft_cfg = MODEL_TARGETS["e2b"]

    print(f"  Target model:  {args.target} ({target_cfg['model']})")
    print(f"  Draft model:   e2b ({draft_cfg['model']})")
    print(f"  Training data: {args.data}")
    print(f"  Strategy:      Mirror target's style distribution for high acceptance rate")
    print(f"{'='*60}\n")

    data_dir = Path(args.data)
    if not (data_dir / "train.jsonl").exists():
        print(f"ERROR: {data_dir / 'train.jsonl'} not found.")
        sys.exit(1)

    draft_adapter = Path.home() / ".human" / "training-data" / "adapters" / "seth-lora-e2b-draft"

    was_running = stop_mlx_server()

    draft_args = argparse.Namespace(
        model=draft_cfg["model"],
        target="e2b",
        data=args.data,
        adapter_path=str(draft_adapter),
        iters=draft_cfg["default_iters"],
        batch_size=draft_cfg["default_batch_size"],
        learning_rate=draft_cfg["default_lr"],
        rank=draft_cfg["default_rank"],
        num_layers=draft_cfg["default_num_layers"],
        max_seq_length=draft_cfg["default_max_seq_length"],
        # Sprint 7 / US-7.4: inherit the operator's target-module choice
        # (or None to fall through to DEFAULT_TARGET_MODULES inside the
        # helper). Without this, the draft model would silently fall back
        # to an undefined attribute on the Namespace.
        target_modules=getattr(args, "target_modules", None),
        # Sprint 11 / US-11.2: forward operator's --train-type choice so
        # the draft model uses the SAME adapter type as the target. If
        # the target trains DoRA but the draft trains LoRA, speculative
        # decode acceptance drops and downstream merge-back diverges.
        train_type=getattr(args, "train_type", "dora"),
        # Sprint 11 / US-11.3: forward early-stopping signal so the draft
        # model uses the same plateau-break detection as the target.
        early_stopping_signal=getattr(args, "early_stopping_signal", "none"),
        steps_per_report=args.steps_per_report,
        steps_per_eval=args.steps_per_eval,
        save_every=args.save_every,
        mask_prompt=True,
        resume=False,
        dpo=args.dpo,
        sft_only=args.sft_only,
        from_corrections=getattr(args, "from_corrections", False),
        no_version=True,
        no_restart_server=True,
    )

    rc = run_sft(draft_args, data_dir, draft_adapter)
    if rc != 0:
        print(f"  Draft model training FAILED")
        if was_running:
            start_mlx_server(resolve_model(args))
        return rc

    if args.dpo and not args.sft_only:
        run_dpo(draft_args, draft_adapter)

    print(f"\n  Draft model ready: {draft_adapter}")
    print(f"  Use with: python3 scripts/mlx-server.py --speculative-draft {draft_adapter}")

    if was_running:
        start_mlx_server(resolve_model(args))

    return 0


def run_quantize(args, adapter_dir: Path) -> int:
    """Quantize the fine-tuned model with calibration-aware quantization.

    Uses mlx_lm.convert with calibrated quantization that preserves PLE
    (ScaledLinear) layers in Gemma 4 models. This produces a fused model
    (base + LoRA merged) with proper quantization that avoids the broken
    mlx-community quants.

    Supports exporting to:
      - MLX format (for mlx-server.py)
      - GGUF format (for llama.cpp / Ollama)
    """
    model = resolve_model(args)
    target_cfg = get_target_config(args.target)

    quant_bits = getattr(args, "quant_bits", 4)
    quant_format = getattr(args, "quant_format", "mlx")
    output_dir = adapter_dir.parent / f"{target_cfg['adapter_suffix']}-q{quant_bits}"

    print(f"\n  === Calibrated Quantization ({quant_bits}-bit, {quant_format}) ===")
    print(f"  Base model:     {model}")
    print(f"  Adapter:        {adapter_dir}")
    print(f"  Output:         {output_dir}")
    print(f"  Format:         {quant_format}")
    print(f"  Bits:           {quant_bits}")

    # PLE-safe: skip quantization of ScaledLinear layers (Gemma 4 specific)
    ple_exclude = "model.layers.*.mlp.gate_proj,model.layers.*.mlp.up_proj"
    if "gemma-4" in model.lower() or "gemma4" in model.lower():
        print(f"  PLE-safe:       YES (excluding ScaledLinear from quantization)")
    else:
        ple_exclude = ""

    if quant_format == "mlx":
        cmd = [
            sys.executable, "-m", "mlx_lm", "convert",
            "--hf-path", model,
            "--mlx-path", str(output_dir),
            "--quantize",
            "--q-bits", str(quant_bits),
            "--q-group-size", "64",
        ]
        if (adapter_dir / "adapters.safetensors").exists():
            cmd.extend(["--adapter-path", str(adapter_dir)])
        if ple_exclude:
            cmd.extend(["--q-exclude", ple_exclude])

        print(f"\n  Running: {' '.join(cmd[:6])}...")
        t0 = time.time()
        result = subprocess.run(cmd, cwd=str(Path(__file__).parent.parent))
        elapsed = time.time() - t0

        if result.returncode == 0:
            print(f"  Quantization complete! ({elapsed/60:.1f} minutes)")
            if output_dir.exists():
                total_size = sum(f.stat().st_size for f in output_dir.rglob("*") if f.is_file())
                print(f"  Output size: {total_size / (1024**3):.2f} GB")
                print(f"  Serve with: python3 scripts/mlx-server.py --model {output_dir}")
        else:
            print(f"  Quantization FAILED (exit code {result.returncode})")
        return result.returncode

    elif quant_format == "gguf":
        print(f"\n  GGUF export requires llama.cpp's convert tool.")
        gguf_output = output_dir.with_suffix(".gguf")

        # First fuse the adapter into the base model
        fused_dir = adapter_dir.parent / f"{target_cfg['adapter_suffix']}-fused"
        cmd_fuse = [
            sys.executable, "-m", "mlx_lm", "fuse",
            "--model", model,
            "--adapter-path", str(adapter_dir),
            "--save-path", str(fused_dir),
            "--de-quantize",
        ]
        print(f"  Step 1: Fusing adapter into base model...")
        result = subprocess.run(cmd_fuse, cwd=str(Path(__file__).parent.parent))
        if result.returncode != 0:
            print(f"  Fuse FAILED")
            return result.returncode

        # Then convert to GGUF using llama.cpp's convert script
        llama_convert = Path.home() / ".local" / "llama.cpp" / "convert_hf_to_gguf.py"
        if not llama_convert.exists():
            for candidate in ["/opt/homebrew/share/llama.cpp/convert_hf_to_gguf.py",
                              Path.home() / "llama.cpp" / "convert_hf_to_gguf.py"]:
                if Path(candidate).exists():
                    llama_convert = Path(candidate)
                    break

        if llama_convert.exists():
            cmd_gguf = [
                sys.executable, str(llama_convert),
                str(fused_dir),
                "--outfile", str(gguf_output),
                "--outtype", f"q{quant_bits}_0",
            ]
            print(f"  Step 2: Converting to GGUF ({quant_bits}-bit)...")
            result = subprocess.run(cmd_gguf)
            if result.returncode == 0:
                size = gguf_output.stat().st_size / (1024**3)
                print(f"  GGUF created: {gguf_output} ({size:.2f} GB)")
                print(f"  Serve with: ./scripts/llamacpp-serve.sh --gguf {gguf_output}")
            else:
                print(f"  GGUF conversion FAILED")
            return result.returncode
        else:
            print(f"  WARNING: llama.cpp convert script not found at {llama_convert}")
            print(f"  Install llama.cpp: ./scripts/llamacpp-serve.sh --build")
            print(f"  Fused model saved at: {fused_dir}")
            return 1

    print(f"  Unknown format: {quant_format}")
    return 1


def run_finetune(args):
    target_cfg = get_target_config(args.target)
    model = resolve_model(args)
    adapter_path = resolve_adapter_path(args)

    print(f"\n{'='*60}")
    print(f"  Gemma 4 LoRA Fine-Tune Pipeline")
    print(f"{'='*60}")
    print(f"  Target:      {args.target} — {target_cfg['description']}")
    print(f"  Model:       {model}")
    print(f"  Data:        {args.data}")
    print(f"  Adapter:     {adapter_path}")
    print(f"  Iterations:  {args.iters}")
    print(f"  Batch size:  {args.batch_size}")
    print(f"  LoRA rank:   {args.rank}")
    # Sprint 11 / US-11.2 observability: surface the active train_type
    # so operators see at-a-glance whether DoRA or LoRA is in use.
    print(f"  Train type:  {getattr(args, 'train_type', 'dora')}")
    # Sprint 7 / US-7.4 risk #2: surface the resolved target modules so
    # the operator can spot a typo (e.g. --target-modules q_proj that
    # collapses the adapter to Q-only) before training starts.
    _resolved_modules = list(
        getattr(args, "target_modules", None) or DEFAULT_TARGET_MODULES
    )
    print(f"  Target mods: {','.join(_resolved_modules)}")
    print(f"  LoRA layers: {args.num_layers}")
    print(f"  LR:          {args.learning_rate}")
    print(f"  Seq length:  {args.max_seq_length}")
    print(f"  Mask prompt: {args.mask_prompt}")
    print(f"  DPO pass:    {'yes' if args.dpo else 'no'}")
    print(f"  Versioning:  {'yes' if not args.no_version else 'no'}")
    if args.speculative_draft:
        print(f"  Spec draft:  will train E2B draft model after target")
    if getattr(args, "quantize", False):
        print(f"  Quantize:    {args.quant_bits}-bit {args.quant_format} (PLE-safe)")
    print(f"{'='*60}\n")

    data_dir = Path(args.data)
    if not (data_dir / "train.jsonl").exists():
        print(f"ERROR: {data_dir / 'train.jsonl'} not found.")
        print(f"Run: python3 scripts/prepare-finetune.py --persona seth --include-chatdb --output {data_dir}")
        sys.exit(1)

    train_count = sum(1 for _ in open(data_dir / "train.jsonl"))
    valid_count = sum(1 for _ in open(data_dir / "valid.jsonl")) if (data_dir / "valid.jsonl").exists() else 0
    print(f"  Training examples: {train_count}")
    print(f"  Validation examples: {valid_count}\n")

    was_running = stop_mlx_server()

    adapter_dir = Path(adapter_path)

    # Phase 1: SFT
    print(f"\n  === Phase 1: Supervised Fine-Tuning ({args.target}) ===\n")
    rc = run_sft(args, data_dir, adapter_dir)
    if rc != 0:
        if was_running and not args.no_restart_server:
            start_mlx_server(model)
        return rc

    # Phase 2: DPO (optional)
    if args.dpo and not args.sft_only:
        print(f"\n  === Phase 2: Direct Preference Optimization ===\n")
        run_dpo(args, adapter_dir)

    # Phase 3: Version the adapter
    lora_config = {
        "target": args.target,
        "model": model,
        "adapter_path": str(adapter_dir),
        "rank": args.rank,
        # Sprint 11 / US-11.2 (AC-11.2.5): record train_type so a
        # downstream consumer can distinguish a DoRA-produced adapter
        # from a LoRA-produced one without parsing the safetensors
        # weights. Defaults to "dora" for runs predating the flag
        # (synthesized Namespaces without train_type set).
        "train_type": getattr(args, "train_type", "dora"),
        # Sprint 7 / US-7.4 (AC-7.4.5): record the resolved target
        # modules alongside rank so a downstream consumer (e.g. the
        # US-7.5 nightly cron's promotion gate or check-lora-ab.sh) can
        # distinguish a true regression in size_mb from a deliberate
        # config change.
        "target_modules": list(
            getattr(args, "target_modules", None) or DEFAULT_TARGET_MODULES
        ),
        "iters": args.iters,
        "batch_size": args.batch_size,
        "learning_rate": args.learning_rate,
        "max_seq_length": args.max_seq_length,
        "dpo": args.dpo,
        "data": str(data_dir),
        "train_examples": train_count,
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
    }

    if not args.no_version:
        print(f"\n  === Phase 3: Adapter Versioning ===\n")
        version_adapter(adapter_dir, lora_config)
    else:
        with open(adapter_dir / "train_config.json", "w") as f:
            json.dump(lora_config, f, indent=2)

    # Phase 4: Calibrated quantization (optional)
    if getattr(args, "quantize", False):
        print(f"\n  === Phase 4: Calibrated Quantization (PLE-safe) ===\n")
        run_quantize(args, adapter_dir)

    # Phase 5: Speculative draft model (optional)
    if args.speculative_draft and args.target != "e2b":
        print(f"\n  === Phase 5: Speculative Draft Model (E2B) ===\n")
        run_speculative_draft_training(args)

    # Phase 6: Restart server
    print(f"\n{'='*60}")
    if (adapter_dir / "adapters.safetensors").exists():
        print(f"  Pipeline complete!")
        print(f"  Adapter: {adapter_dir}")
        if (adapter_dir / "adapters.safetensors").exists():
            size_mb = (adapter_dir / "adapters.safetensors").stat().st_size / (1024 * 1024)
            print(f"  Size: {size_mb:.1f} MB")

        if not args.no_restart_server:
            start_mlx_server(model, str(adapter_dir))

        print(f"\n  Test it: curl http://127.0.0.1:{MLX_SERVER_PORT}/v1/chat/completions \\")
        print(f'    -d \'{{"messages":[{{"role":"user","content":"hey whats up"}}]}}\'')

        if args.target in ("e4b", "e2b"):
            print(f"\n  Real-time voice: python3 scripts/mlx-server.py --model {model} --adapter-path {adapter_dir}")
            print(f"  Benchmark: python3 scripts/voice-bench.py --endpoint http://127.0.0.1:{MLX_SERVER_PORT}")
    else:
        print(f"  Fine-tuning FAILED — no adapter produced")
        if was_running and not args.no_restart_server:
            start_mlx_server(model)
    print(f"{'='*60}\n")

    return 0


def run_train_all(args):
    """Train all three model targets on the same data for a complete real-time stack."""
    print(f"\n{'='*60}")
    print(f"  FULL REAL-TIME STACK TRAINING")
    print(f"  Training 31B + E4B + E2B on the same persona data")
    print(f"{'='*60}\n")

    targets = ["e4b", "e2b", "31b"] if args.realtime_first else ["31b", "e4b", "e2b"]

    for target in targets:
        target_cfg = MODEL_TARGETS[target]
        print(f"\n  {'='*50}")
        print(f"  Training {target}: {target_cfg['description']}")
        print(f"  {'='*50}\n")

        target_args = argparse.Namespace(
            target=target,
            model=None,
            data=args.data,
            adapter_path=None,
            iters=target_cfg["default_iters"],
            batch_size=target_cfg["default_batch_size"],
            learning_rate=target_cfg["default_lr"],
            rank=target_cfg["default_rank"],
            num_layers=target_cfg["default_num_layers"],
            max_seq_length=target_cfg["default_max_seq_length"],
            # Sprint 7 / US-7.4: forward operator's target-module choice
            # (or None for the QKVO default) into each per-target run.
            target_modules=getattr(args, "target_modules", None),
            # Sprint 11 / US-11.2: forward --train-type so all three
            # targets train with the same adapter type. Mixing DoRA and
            # LoRA across the real-time stack would diverge magnitudes.
            train_type=getattr(args, "train_type", "dora"),
            # Sprint 11 / US-11.3: forward early-stopping signal so each
            # per-target run honors the same plateau-break policy.
            early_stopping_signal=getattr(args, "early_stopping_signal", "none"),
            steps_per_report=args.steps_per_report,
            steps_per_eval=args.steps_per_eval,
            save_every=args.save_every,
            mask_prompt=True,
            resume=False,
            dpo=args.dpo,
            sft_only=args.sft_only,
            from_corrections=getattr(args, "from_corrections", False),
            no_version=False,
            no_restart_server=True,
            speculative_draft=False,
        )

        rc = run_finetune(target_args)
        if rc != 0:
            print(f"\n  WARNING: {target} training failed (exit code {rc}), continuing...")

    e4b_adapter = Path.home() / ".human" / "training-data" / "adapters" / "seth-lora-e4b"
    e2b_adapter = Path.home() / ".human" / "training-data" / "adapters" / "seth-lora-e2b"

    print(f"\n{'='*60}")
    print(f"  FULL STACK TRAINING COMPLETE")
    print(f"{'='*60}")
    print(f"\n  Real-time voice (E4B + E2B speculative):")
    print(f"    python3 scripts/mlx-server.py \\")
    print(f"      --model {MODEL_TARGETS['e4b']['model']} \\")
    print(f"      --adapter-path {e4b_adapter} \\")
    print(f"      --speculative-draft-adapter {e2b_adapter}")
    print(f"\n  High-quality (31B):")
    print(f"    python3 scripts/mlx-server.py --adapter-path ~/.human/training-data/adapters/seth-lora")
    print(f"\n  Benchmark:")
    print(f"    python3 scripts/voice-bench.py --endpoint http://127.0.0.1:{MLX_SERVER_PORT}")
    print(f"{'='*60}\n")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Fine-tune Gemma 4 models with LoRA on MLX",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Model targets:
  31b    Dense 31B — highest quality, ~20 tok/s (default)
  e4b    Edge 4B  — native audio, ~110 tok/s, real-time voice
  e2b    Edge 2B  — speculative decode draft, ~180 tok/s

Examples:
  %(prog)s --data ~/.human/training-data/finetune
  %(prog)s --target e4b --data ~/.human/training-data/finetune
  %(prog)s --target e4b --speculative-draft --data ~/.human/training-data/finetune
  %(prog)s --train-all --data ~/.human/training-data/finetune
""")
    parser.add_argument("--target", choices=list(MODEL_TARGETS.keys()), default=DEFAULT_TARGET,
                        help="Model target: 31b (default), e4b (real-time voice), e2b (draft)")
    parser.add_argument("--model", default=None,
                        help="Override base model (default: auto from target)")
    parser.add_argument("--data", default=DEFAULT_DATA, help="Training data directory")
    parser.add_argument("--adapter-path", default=None,
                        help="Where to save the LoRA adapter (default: auto from target)")
    parser.add_argument("--iters", type=int, default=None,
                        help="Training iterations (default: auto from target)")
    parser.add_argument("--batch-size", type=int, default=None,
                        help="Batch size (default: auto from target)")
    parser.add_argument("--learning-rate", type=float, default=None,
                        help="Learning rate (default: auto from target)")
    parser.add_argument("--rank", type=int, default=None, help="LoRA rank (default: auto from target)")
    # Sprint 11 / US-11.2: adapter training type. Defaults to "dora" —
    # DoRA closes ~93% of the LoRA-to-full-FT quality gap at the same
    # rank with no inference overhead after merge-back. Operators with
    # prior LoRA adapters in adapters/ should either delete them or
    # pass `--train-type lora` to keep the old shape.
    parser.add_argument("--train-type", choices=["dora", "lora"], default="dora",
                        help="Adapter type: dora (default, US-11.2) or lora "
                             "for back-compat. DoRA closes ~93%% of the "
                             "LoRA-to-full-FT quality gap at the same rank "
                             "with no inference overhead.")
    # Sprint 7 / US-7.4: CSV of LoRA target module names. CSV is the
    # friendliest input — we materialize it into the `keys` field of the
    # `--lora-parameters` JSON token at subprocess-build time. The
    # canonical set for Gemma/Llama/Qwen/Mistral attention is QKVO; the
    # MLP set (gate_proj,up_proj,down_proj) is opt-in.
    parser.add_argument("--target-modules", type=str, default=None,
                        help="CSV of LoRA target module names "
                             "(default: q_proj,k_proj,v_proj,o_proj). "
                             "Passed via --lora-parameters JSON `keys` to mlx-lm/mlx-lm-lora.")
    parser.add_argument("--num-layers", type=int, default=None,
                        help="Number of layers to apply LoRA (default: auto from target)")
    parser.add_argument("--max-seq-length", type=int, default=None,
                        help="Max sequence length (default: auto from target)")
    parser.add_argument("--steps-per-report", type=int, default=5, help="Report every N steps")
    parser.add_argument("--steps-per-eval", type=int, default=20, help="Evaluate every N steps")
    parser.add_argument("--save-every", type=int, default=20, help="Save checkpoint every N steps")
    parser.add_argument("--mask-prompt", action="store_true", default=True,
                        help="Mask prompt tokens (only train on responses, default: true)")
    parser.add_argument("--no-mask-prompt", action="store_false", dest="mask_prompt")
    parser.add_argument("--dpo", action="store_true", default=True,
                        help="Run DPO pass after SFT if DPO data exists (default: true)")
    parser.add_argument("--no-dpo", action="store_false", dest="dpo",
                        help="Disable the DPO pass entirely")
    parser.add_argument("--sft-only", action="store_true",
                        help="Skip DPO even if data exists (alias for --no-dpo intent; "
                             "stronger because it also short-circuits the speculative-draft DPO pass)")
    parser.add_argument("--from-corrections", action="store_true",
                        help="Load DPO pairs from ~/.human/dpo/pairs.jsonl (LOCKED path; "
                             "produced by the outbound-corrections miner — US-7.2)")
    parser.add_argument("--no-version", action="store_true", help="Don't version the adapter")
    parser.add_argument("--resume", action="store_true", help="Resume from existing adapter")
    parser.add_argument("--no-restart-server", action="store_true", help="Don't restart MLX server after training")
    parser.add_argument("--speculative-draft", action="store_true",
                        help="Also train an E2B draft model for speculative decoding")
    parser.add_argument("--quantize", action="store_true",
                        help="Run PLE-safe calibrated quantization after training")
    parser.add_argument("--quant-bits", type=int, default=4, choices=[2, 3, 4, 8],
                        help="Quantization bit width (default: 4)")
    parser.add_argument("--quant-format", choices=["mlx", "gguf"], default="mlx",
                        help="Quantization output format: mlx (default) or gguf (for llama.cpp/Ollama)")
    parser.add_argument("--train-all", action="store_true",
                        help="Train all three targets (31B + E4B + E2B) on the same data")
    parser.add_argument("--realtime-first", action="store_true",
                        help="With --train-all, train E4B and E2B before 31B (get real-time running fast)")
    # Sprint 11 / US-11.3 — chosen_r plateau-break early stopping.
    # Default 'chosen_r' for new operator-driven runs (Sprint 11 baseline).
    # Pass 'none' to disable (AC-11.3.5 back-compat for Sprint 7/8 CI scripts
    # and any pipeline that wants to run to the full --iters horizon).
    parser.add_argument("--early-stopping-signal", choices=["chosen_r", "none"],
                        default="chosen_r",
                        help="DPO early-stopping signal: chosen_r (default, "
                             "US-11.3; halts when train_chosen_r plateau breaks "
                             "by >50%% drop for 2 consecutive eval steps) or "
                             "none (run to --iters, Sprint 7/8 behavior).")
    args = parser.parse_args()

    target_cfg = get_target_config(args.target)
    if args.iters is None:
        args.iters = target_cfg["default_iters"]
    if args.batch_size is None:
        args.batch_size = target_cfg["default_batch_size"]
    if args.learning_rate is None:
        args.learning_rate = target_cfg["default_lr"]
    if args.rank is None:
        args.rank = target_cfg["default_rank"]
    if args.num_layers is None:
        args.num_layers = target_cfg["default_num_layers"]
    if args.max_seq_length is None:
        args.max_seq_length = target_cfg["default_max_seq_length"]

    # Sprint 7 / US-7.4: parse --target-modules CSV into list[str] exactly
    # once, here in main(), so run_sft/run_dpo never re-parse and never see
    # a None or a string. Default to QKVO when the flag is absent.
    if args.target_modules is None:
        args.target_modules = list(DEFAULT_TARGET_MODULES)
    else:
        # Operator passed --target-modules explicitly. Parse and reject an
        # empty result outright: if the user typed `--target-modules ,` or
        # `--target-modules " , "` the parsed list collapses to [], which
        # would silently fall back to QKVO inside _build_lora_parameters_json
        # via `or DEFAULT_TARGET_MODULES`. That fallback is fine as an
        # internal guard for synthetic Namespaces (run_train_all,
        # run_speculative_draft_training pass target_modules=None) but it
        # MUST NOT mask an operator typo on the CLI — fail loudly (HIGH-1).
        parsed = [s.strip() for s in args.target_modules.split(",") if s.strip()]
        if not parsed:
            sys.exit(
                "ERROR: --target-modules produced an empty list after parsing; "
                "check for stray commas"
            )
        args.target_modules = parsed

    if args.train_all:
        sys.exit(run_train_all(args))

    sys.exit(run_finetune(args))


if __name__ == "__main__":
    main()
