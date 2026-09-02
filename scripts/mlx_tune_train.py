#!/usr/bin/env python3
# scripts/mlx_tune_train.py
#
# Contract C6: an mlx-tune (https://github.com/ARahim3/mlx-tune, v0.6,
# reference-free SimPO/KTO/ORPO with per-expert LoRA for MoE) driver for
# h-uman's GLM preference-training pipeline. Invoked by
# scripts/train-glm-adapter.sh via `--trainer mlx_tune --train-mode
# simpo|kto|orpo`.
#
# Runs in its OWN venv: ~/.human/venvs/mlxtune312 (NOT ~/.human/venvs/train312,
# which stays exclusively mlx_lm/mlx_lm_lora). See scripts/mlx_tune_env.txt
# for exactly what's installed and why, including the architecture-support
# finding this file's --dry-run reproduces live.
#
# Reads the SAME YAML config shape as ~/.human/training-data/glm-v61-orpo-
# config.yaml (model, data, lora_parameters{rank,scale,dropout}, num_layers,
# max_seq_length, batch_size, iters, learning_rate) and, for the real
# (non-dry-run) path: builds an mlx-tune SimPO/KTO/ORPO trainer, applies LoRA
# with per-expert MoE targeting, trains, and writes an mlx_lm-compatible
# adapters.safetensors + adapter_config.json to --adapter-out -- asserting
# lora_b non-zero afterward exactly like scripts/train-glm-adapter.sh's
# existing no-op guard (see .claude/rules/lora-scale-default-or-die.md and
# .claude/rules/reports-success-does-nothing.md).
#
# NO-GO for this contract: --dry-run must NEVER load model weights. It reads
# config.json (a few KB, already cached locally) and imports the mlx_lm
# python module for the resolved model_type -- both zero-weight operations --
# to answer "is this architecture supported" without touching the 56 GB base.
# The real (non-dry-run) path DOES load the base and is gated behind the
# HU_MLX_TUNE_ALLOW_LOAD=1 env var so a bare `python mlx_tune_train.py
# --config ...` invocation (skipping train-glm-adapter.sh's prod-stop /
# headroom / reap sequence) refuses rather than silently loading a second
# 56 GB model beside a possibly-still-running production server.
"""mlx-tune SimPO/KTO/ORPO driver for the GLM preference-training pipeline."""

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

import yaml

# --------------------------------------------------------------------------
# Config translation
# --------------------------------------------------------------------------


def load_yaml_config(path: str) -> dict:
    """Read the mlx_lm-shaped YAML config (same file the mlx_lm / mlx_lm_lora
    trainers already read via scripts/train-glm-adapter.sh's --config)."""
    p = Path(path)
    if not p.is_file():
        raise FileNotFoundError(f"config not found: {path}")
    with open(p, "r") as f:
        cfg = yaml.safe_load(f)
    if not isinstance(cfg, dict):
        raise ValueError(f"config did not parse to a mapping: {path}")
    return cfg


def require_lora_parameters(cfg: dict) -> dict:
    """Extract + validate lora_parameters exactly like train-glm-adapter.sh's
    preflight grep for 'lora_parameters:' + nested 'scale: 2.0' -- but as a
    real parse, not a regex over the YAML text.

    See .claude/rules/lora-scale-default-or-die.md: mlx_lm defaults to
    scale=20.0, mlx-lm-lora to scale=10.0 -- both catastrophic. This contract
    additionally REQUIRES scale to be explicit 2.0, matching the convention
    every GLM adapter (v5, v6, v6.1, v6.2) in this repo has shipped with.
    """
    lp = cfg.get("lora_parameters")
    if not isinstance(lp, dict):
        raise ValueError(
            "config has no top-level 'lora_parameters' mapping -- flat "
            "'scale:'/'rank:' keys are silently ignored by both mlx_lm and "
            "mlx-tune's own translation layer"
        )
    for key in ("rank", "scale", "dropout"):
        if key not in lp:
            raise ValueError(f"lora_parameters missing required key: {key}")
    scale = float(lp["scale"])
    if scale != 2.0:
        raise ValueError(
            f"lora_parameters.scale={scale!r}, expected exactly 2.0 -- "
            "refusing (lora-scale-default-or-die)"
        )
    return lp


def lora_alpha_from_scale(rank: int, scale: float) -> float:
    """mlx-tune's FastLanguageModel.get_peft_model() takes lora_alpha, not
    scale, and internally computes scale = lora_alpha / r (mlx_tune/model.py
    MLXModelWrapper._apply_lora). To land on our required scale=2.0 for a
    given rank, invert that: alpha = rank * scale. For rank=8, scale=2.0 this
    is alpha=16 -- the same alpha=2*rank convention the project's own
    lora-scale rule documents as the sane PEFT default.
    """
    return rank * scale


# --------------------------------------------------------------------------
# Architecture support -- zero weight loading
# --------------------------------------------------------------------------


def resolve_architecture_support(model_id: str) -> dict:
    """Answer "does mlx-tune support this architecture" WITHOUT loading any
    model weights.

    mlx-tune has no explicit named "supported architectures" registry for
    text LLMs (verified: grepping mlx_tune/*.py for SUPPORTED/ARCH_REGISTRY/
    registry/ARCHITECTURES turns up nothing LLM-related -- only OCR/
    embeddings/audio submodules have their own small per-feature allowlists).
    Text-model support is inherited entirely from mlx_lm: mlx-tune loads
    via `mlx_lm.load()`, which dispatches on config.json's "model_type" to
    `mlx_lm.models.<model_type>` via `mlx_lm.utils._get_classes()` -- an
    IMPORT, not a weight load -- and applies LoRA (including per-expert MoE
    LoRA) via dynamic introspection over the model's actual module types
    (mlx_tune.model._resolve_target_modules), not a name-keyed table.

    So "is the architecture supported" reduces to: (1) can mlx_lm resolve a
    Model/ModelArgs pair for this model_type at all, and (2) does that
    module expose SwitchLinear-based MoE layers (for the "per-expert LoRA"
    half of this contract) alongside dense ones. Both are answerable by
    reading config.json + importing one python module -- no safetensors
    touched.
    """
    from huggingface_hub import try_to_load_from_cache
    from mlx_lm.utils import _get_classes

    result = {
        "model_id": model_id,
        "config_source": None,
        "model_type": None,
        "mlx_lm_module_importable": False,
        "mlx_lm_module_file": None,
        "has_switch_linear_moe": False,
        "supported": False,
        "note": None,
    }

    config_path = None
    local_dir = Path(model_id)
    if local_dir.is_dir() and (local_dir / "config.json").is_file():
        config_path = local_dir / "config.json"
        result["config_source"] = f"local path: {config_path}"
    else:
        # HF hub id: look ONLY in the local cache. try_to_load_from_cache
        # never makes a network request -- if the repo isn't cached, this
        # returns None and we say so rather than downloading, since a
        # dry-run must not depend on network access either.
        cached = try_to_load_from_cache(repo_id=model_id, filename="config.json")
        if cached and Path(cached).is_file():
            config_path = Path(cached)
            result["config_source"] = f"HF hub cache: {config_path}"

    if config_path is None:
        result["note"] = (
            f"config.json for {model_id!r} not found in the local HF hub "
            "cache and no network fetch was attempted (dry-run must not "
            "require network access). Cannot determine architecture "
            "support offline -- run once with network access to populate "
            "the cache (e.g. via mlx_lm's own -c config.yaml dry validation),"
            " or point --config at a model with a locally cached config.json."
        )
        return result

    with open(config_path, "r") as f:
        config = json.load(f)
    result["model_type"] = config.get("model_type")

    try:
        model_cls, args_cls = _get_classes(config)
        result["mlx_lm_module_importable"] = True
        result["mlx_lm_module_file"] = model_cls.__module__
    except ValueError as e:
        result["note"] = f"mlx_lm._get_classes() refused: {e}"
        return result

    # Per-expert MoE LoRA support: does the resolved module define/import
    # SwitchLinear (mlx_lm.models.switch_layers)? This is what mlx-tune's
    # _resolve_target_modules scans for at LoRA-application time.
    import importlib

    mod = importlib.import_module(model_cls.__module__)
    src_path = Path(mod.__file__)
    src = src_path.read_text()
    result["has_switch_linear_moe"] = "SwitchLinear" in src or "SwitchGLU" in src

    result["supported"] = (
        result["mlx_lm_module_importable"] and result["has_switch_linear_moe"]
    )
    if result["supported"]:
        result["note"] = (
            f"model_type={result['model_type']!r} resolves to "
            f"{model_cls.__module__}.{model_cls.__name__}, which builds "
            "MoE experts on mlx_lm.models.switch_layers.SwitchLinear -- "
            "mlx-tune's dynamic target-module resolver "
            "(mlx_tune.model._resolve_target_modules) will target every "
            "routed-expert gate/up/down projection plus any dense "
            "shared-expert / early-layer projections in the same pass."
        )
    elif result["mlx_lm_module_importable"]:
        result["note"] = (
            f"model_type={result['model_type']!r} imports fine but its "
            "module has no SwitchLinear/SwitchGLU reference -- this is a "
            "DENSE architecture as far as mlx-tune's MoE detection is "
            "concerned. Standard (non-per-expert) LoRA would still apply; "
            "the per-expert-MoE half of this contract does not apply to it."
        )
    return result


# --------------------------------------------------------------------------
# Data validation
# --------------------------------------------------------------------------

REQUIRED_PAIR_KEYS = ("prompt", "chosen", "rejected")


def _count_and_validate_jsonl(path: Path) -> dict:
    if not path.is_file():
        return {"path": str(path), "exists": False, "count": 0, "bad_lines": []}
    count = 0
    bad_lines = []
    with open(path, "r") as f:
        for lineno, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            count += 1
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as e:
                bad_lines.append((lineno, f"invalid JSON: {e}"))
                continue
            missing = [k for k in REQUIRED_PAIR_KEYS if k not in obj]
            if missing:
                bad_lines.append((lineno, f"missing keys: {missing}"))
    return {
        "path": str(path),
        "exists": True,
        "count": count,
        "bad_lines": bad_lines[:10],  # cap -- this is a report, not a dump
        "bad_line_total": len(bad_lines),
    }


def validate_data_dir(data_dir: str) -> dict:
    d = Path(data_dir)
    train_report = _count_and_validate_jsonl(d / "train.jsonl")
    valid_report = _count_and_validate_jsonl(d / "valid.jsonl")
    ok = (
        train_report["exists"]
        and train_report["count"] > 0
        and train_report["bad_line_total"] == 0
        and (not valid_report["exists"] or valid_report["bad_line_total"] == 0)
    )
    return {"data_dir": str(d), "train": train_report, "valid": valid_report, "ok": ok}


def load_preference_pairs(jsonl_path: Path) -> list:
    pairs = []
    with open(jsonl_path, "r") as f:
        for line in f:
            line = line.strip()
            if line:
                pairs.append(json.loads(line))
    return pairs


def to_kto_examples(pairs: list) -> list:
    """mlx-tune's KTOTrainer does NOT accept {prompt, chosen, rejected} --
    its train() only recognizes TRL format {prompt, completion, label} or
    legacy {text, label} (mlx_tune/rl_trainers.py KTOTrainer.train(): any
    sample missing both shapes hits `continue`, so passing our pairs
    straight through silently drops every example and then crashes with a
    ZeroDivisionError on an empty tokenized_data list -- a loud crash here,
    not a silent no-op, but still the wrong dataset shape).

    Translate each (prompt, chosen, rejected) pair into the two labeled
    examples KTO expects: the chosen completion is desirable (label=True),
    the rejected completion is undesirable (label=False).
    """
    examples = []
    for pair in pairs:
        prompt = pair["prompt"]
        examples.append({"prompt": prompt, "completion": pair["chosen"], "label": True})
        examples.append({"prompt": prompt, "completion": pair["rejected"], "label": False})
    return examples


# --------------------------------------------------------------------------
# The no-op guard -- reused verbatim in spirit from train-glm-adapter.sh
# --------------------------------------------------------------------------


def assert_lora_b_nonzero(adapter_dir: Path) -> None:
    """Mirrors the heredoc guard in scripts/train-glm-adapter.sh: LoRA
    computes out = x@W + scale*(x@A)@B, B is zero-initialized, so an adapter
    whose every lora_b is still 0.0 after "training" is a mathematically
    exact no-op -- a green exit code, a moving loss curve, and a saved
    .safetensors file all fail to reveal this (see
    .claude/rules/reports-success-does-nothing.md). Raises SystemExit(1) on
    failure, exactly like the bash guard's `die`.
    """
    import mlx.core as mx

    st_path = adapter_dir / "adapters.safetensors"
    if not st_path.is_file():
        sys.exit(f"[mlx_tune_train] FATAL: no adapters.safetensors at {adapter_dir}")

    w = mx.load(str(st_path))
    b_keys = [k for k in w if k.endswith("lora_b")]
    if not b_keys:
        sys.exit("[mlx_tune_train] FATAL: no lora_b tensors at all")

    nz = sum(1 for k in b_keys if float(mx.abs(w[k]).max()) > 0)
    mx_abs = max(float(mx.abs(w[k]).max()) for k in b_keys)
    print(f"[mlx_tune_train] lora_b non-zero {nz}/{len(b_keys)}, max|B| = {mx_abs:.3e}")

    if nz == 0:
        sys.exit(
            "[mlx_tune_train] FATAL: every lora_b is 0.0 -> adapter == base "
            "model, nothing was learned"
        )
    if nz < len(b_keys) // 2:
        sys.exit(
            f"[mlx_tune_train] FATAL: only {nz}/{len(b_keys)} lora_b are "
            "non-zero -- partial or corrupt training"
        )
    print("[mlx_tune_train] confirmed: the adapter has real weight movement")


# --------------------------------------------------------------------------
# adapter_config.json writer / verifier
# --------------------------------------------------------------------------


def write_or_verify_adapter_config(
    adapter_dir: Path, rank: int, scale: float, dropout: float, num_layers: int
) -> dict:
    """After mlx-tune's own trainer.train() -> _save_adapters_and_config()
    has run (it computes lora_parameters.scale = lora_alpha / rank
    internally -- see mlx_tune/rl_trainers.py), re-read adapter_config.json
    and assert it landed on the scale we asked for. If the file is missing
    the nested block, or the computed scale drifted (float rounding, a
    future mlx-tune version changing the alpha/r convention, etc.), REWRITE
    the nested lora_parameters block to the exact requested values rather
    than trusting mlx-tune's arithmetic -- then re-verify.

    This is defense-in-depth on top of lora_alpha_from_scale()'s inversion:
    if the two ever disagree, prefer the value this contract requires
    (train-glm-adapter.sh's post-training check reads exactly this field --
    see .claude/rules/lora-scale-default-or-die.md).
    """
    cfg_path = adapter_dir / "adapter_config.json"
    if cfg_path.is_file():
        with open(cfg_path, "r") as f:
            cfg = json.load(f)
    else:
        cfg = {"fine_tune_type": "lora"}

    lp = cfg.get("lora_parameters")
    needs_rewrite = (
        not isinstance(lp, dict)
        or lp.get("rank") != rank
        or float(lp.get("scale", -1)) != float(scale)
        or float(lp.get("dropout", -1)) != float(dropout)
    )
    if needs_rewrite:
        existing_keys = lp.get("keys") if isinstance(lp, dict) else None
        cfg["lora_parameters"] = {"rank": rank, "scale": scale, "dropout": dropout}
        if existing_keys:
            cfg["lora_parameters"]["keys"] = existing_keys
        cfg.setdefault("num_layers", num_layers)
        cfg.setdefault("fine_tune_type", "lora")
        with open(cfg_path, "w") as f:
            json.dump(cfg, f, indent=2)
        print(
            f"[mlx_tune_train] rewrote {cfg_path} lora_parameters "
            f"(was {lp!r}, now rank={rank} scale={scale} dropout={dropout})"
        )

    with open(cfg_path, "r") as f:
        final = json.load(f)
    final_scale = float(final["lora_parameters"]["scale"])
    if final_scale != float(scale):
        sys.exit(
            f"[mlx_tune_train] FATAL: adapter_config.json lora_parameters."
            f"scale={final_scale} after write/verify, expected {scale} -- "
            "adapter is NOT safe to serve (lora-scale-default-or-die)"
        )
    print(f"[mlx_tune_train] adapter_config.json lora_parameters.scale = {final_scale}")
    return final


# --------------------------------------------------------------------------
# dry-run
# --------------------------------------------------------------------------


def cmd_dry_run(args: argparse.Namespace) -> int:
    print("=" * 70)
    print("mlx_tune_train.py --dry-run")
    print("=" * 70)

    ok = True

    try:
        cfg = load_yaml_config(args.config)
        print(f"[dry-run] config: {args.config}")
    except Exception as e:
        print(f"[dry-run] FATAL: could not read config: {e}")
        return 1

    model_id = cfg.get("model")
    print(f"[dry-run] model: {model_id}")

    try:
        lp = require_lora_parameters(cfg)
        alpha = lora_alpha_from_scale(int(lp["rank"]), float(lp["scale"]))
        print(
            f"[dry-run] lora_parameters: rank={lp['rank']} scale={lp['scale']} "
            f"dropout={lp['dropout']} -> mlx-tune lora_alpha={alpha} "
            f"(alpha = rank * scale, inverting mlx-tune's scale = alpha / rank)"
        )
    except Exception as e:
        print(f"[dry-run] FATAL: {e}")
        ok = False

    data_dir = cfg.get("data")
    if data_dir:
        report = validate_data_dir(data_dir)
        print(f"[dry-run] data dir: {report['data_dir']}")
        print(
            f"[dry-run]   train.jsonl: {report['train']['count']} pairs "
            f"({report['train']['bad_line_total']} malformed)"
        )
        print(
            f"[dry-run]   valid.jsonl: {report['valid']['count']} pairs "
            f"({report['valid']['bad_line_total']} malformed)"
        )
        if not report["ok"]:
            print("[dry-run] FATAL: data dir failed validation")
            for lineno, msg in report["train"]["bad_lines"]:
                print(f"[dry-run]     train.jsonl:{lineno}: {msg}")
            ok = False
    else:
        print("[dry-run] FATAL: config has no 'data' key")
        ok = False

    if args.train_mode == "kto" and data_dir and Path(data_dir, "train.jsonl").is_file():
        pairs = load_preference_pairs(Path(data_dir, "train.jsonl"))
        kto_examples = to_kto_examples(pairs)
        print(
            f"[dry-run] --train-mode kto: translating {len(pairs)} "
            f"(prompt, chosen, rejected) pairs -> {len(kto_examples)} "
            "TRL-format (prompt, completion, label) examples -- mlx-tune's "
            "KTOTrainer does not accept the chosen/rejected shape directly"
        )

    if model_id:
        arch = resolve_architecture_support(model_id)
        print(f"[dry-run] architecture support for {model_id!r} (ZERO weights loaded):")
        print(f"[dry-run]   config source:            {arch['config_source']}")
        print(f"[dry-run]   model_type:                {arch['model_type']}")
        print(f"[dry-run]   mlx_lm module importable:  {arch['mlx_lm_module_importable']}")
        print(f"[dry-run]   mlx_lm module file:        {arch['mlx_lm_module_file']}")
        print(f"[dry-run]   has SwitchLinear/MoE:      {arch['has_switch_linear_moe']}")
        print(f"[dry-run]   SUPPORTED:                 {arch['supported']}")
        print(f"[dry-run]   note: {arch['note']}")
        if not arch["supported"]:
            ok = False
    else:
        arch = None

    print("-" * 70)
    if ok:
        print("[dry-run] PASS -- config, data, and architecture support all validated")
        print("[dry-run] NOTE: no model weights were loaded during this dry-run")
        return 0
    else:
        print("[dry-run] FAIL -- see FATAL lines above")
        return 1


# --------------------------------------------------------------------------
# real train (NOT exercised by contract C6 -- gated behind an explicit env
# var so this file cannot accidentally trigger a load outside
# train-glm-adapter.sh's guarded sequence)
# --------------------------------------------------------------------------


def cmd_train(args: argparse.Namespace) -> int:
    if os.environ.get("HU_MLX_TUNE_ALLOW_LOAD") != "1":
        sys.exit(
            "[mlx_tune_train] REFUSING to load the base model: "
            "HU_MLX_TUNE_ALLOW_LOAD=1 is not set.\n"
            "This script must be invoked via scripts/train-glm-adapter.sh "
            "--trainer mlx_tune, which stops production, waits for a full "
            "reap, checks memory headroom, and sets this env var only in "
            "that guarded sequence. A bare invocation of this script would "
            "skip all of that and load a second 56 GB model beside a "
            "possibly-still-running production mlx-server -- see "
            "never_two_llm_instances / .claude/rules/lora-scale-default-or-die.md."
        )

    from mlx_tune.model import FastLanguageModel
    from mlx_tune.rl_trainers import (
        ORPOConfig,
        ORPOTrainer,
        KTOConfig,
        KTOTrainer,
        SimPOConfig,
        SimPOTrainer,
    )

    cfg = load_yaml_config(args.config)
    lp = require_lora_parameters(cfg)
    rank = int(lp["rank"])
    scale = float(lp["scale"])
    dropout = float(lp["dropout"])
    alpha = lora_alpha_from_scale(rank, scale)
    num_layers = int(cfg.get("num_layers", 8))
    max_seq_length = int(cfg.get("max_seq_length", 2048))
    batch_size = int(cfg.get("batch_size", 1))
    iters = int(cfg.get("iters", 400))
    learning_rate = float(cfg.get("learning_rate", 5e-6))
    logging_steps = int(cfg.get("steps_per_report", 10))
    save_steps = int(cfg.get("save_every", 100))
    model_id = cfg["model"]
    data_dir = Path(cfg["data"])

    print(f"[mlx_tune_train] loading base model: {model_id}")
    model, tokenizer = FastLanguageModel.from_pretrained(model_id)

    model = FastLanguageModel.get_peft_model(
        model,
        r=rank,
        lora_alpha=alpha,
        lora_dropout=dropout,
    )

    # Apply LoRA ourselves with the config's num_layers BEFORE constructing
    # the trainer. mlx-tune's RL trainers call `model._apply_lora()` (no
    # num_layers arg) internally at the start of train(), which would
    # silently apply LoRA to EVERY layer rather than honoring num_layers --
    # _apply_lora()'s own `_lora_applied` guard makes that call a no-op once
    # we've already applied it here, so this is how num_layers is honored.
    model._apply_lora(num_layers=num_layers)
    if not model._lora_applied:
        sys.exit("[mlx_tune_train] FATAL: LoRA did not apply (see mlx-tune output above)")

    train_pairs_path = data_dir / "train.jsonl"
    train_pairs = load_preference_pairs(train_pairs_path)
    if args.train_mode == "kto":
        train_dataset = to_kto_examples(train_pairs)
    else:
        train_dataset = train_pairs

    work_dir = Path(args.adapter_out).parent / (Path(args.adapter_out).name + ".mlx_tune_work")
    common_kwargs = dict(
        output_dir=str(work_dir),
        learning_rate=learning_rate,
        per_device_train_batch_size=batch_size,
        max_steps=iters,
        logging_steps=logging_steps,
        save_steps=save_steps,
        max_seq_length=max_seq_length,
    )

    if args.train_mode == "simpo":
        trainer = SimPOTrainer(
            model=model,
            train_dataset=train_dataset,
            tokenizer=tokenizer,
            args=SimPOConfig(beta=args.beta, gamma=args.gamma, **common_kwargs),
        )
    elif args.train_mode == "kto":
        trainer = KTOTrainer(
            model=model,
            train_dataset=train_dataset,
            tokenizer=tokenizer,
            args=KTOConfig(beta=args.beta, **common_kwargs),
        )
    elif args.train_mode == "orpo":
        trainer = ORPOTrainer(
            model=model,
            train_dataset=train_dataset,
            tokenizer=tokenizer,
            args=ORPOConfig(beta=args.beta, **common_kwargs),
        )
    else:
        sys.exit(f"[mlx_tune_train] FATAL: unknown --train-mode {args.train_mode!r}")

    print(f"Training Mode: {args.train_mode}")  # matches train-glm-adapter.sh's grep
    trainer.train()

    adapter_out = Path(args.adapter_out)
    adapter_out.mkdir(parents=True, exist_ok=True)
    for name in ("adapters.safetensors", "adapter_config.json"):
        src = trainer.adapter_path / name
        if src.is_file():
            shutil.copy(src, adapter_out / name)
    shutil.rmtree(work_dir, ignore_errors=True)

    write_or_verify_adapter_config(adapter_out, rank, scale, dropout, num_layers)
    assert_lora_b_nonzero(adapter_out)
    print(f"[mlx_tune_train] DONE. adapter={adapter_out}")
    return 0


# --------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--config", required=True, help="mlx_lm-shaped YAML config")
    p.add_argument(
        "--train-mode",
        choices=("simpo", "kto", "orpo"),
        default="orpo",
        help="mlx-tune reference-free preference objective",
    )
    p.add_argument("--beta", type=float, default=0.1)
    p.add_argument("--gamma", type=float, default=0.5, help="SimPO target reward margin")
    p.add_argument("--adapter-out", default=None, help="output dir (required unless --dry-run)")
    p.add_argument("--dry-run", action="store_true")
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    if args.dry_run:
        return cmd_dry_run(args)
    if not args.adapter_out:
        print("[mlx_tune_train] FATAL: --adapter-out is required unless --dry-run")
        return 2
    return cmd_train(args)


if __name__ == "__main__":
    sys.exit(main())
