#!/usr/bin/env python3
# scripts/mlx_lm_sft_templated.py
#
# Runs `mlx_lm.lora` SFT on rows PRE-TEMPLATED by scripts/chat_template_rows.py,
# so a GLM persona adapter trained by supervision sees the exact token stream
# production serves and learns where its turn ends.
#
# WHY (2026-10-02): the nightly retrain's SFT candidate
# (ai.human.nightly-retrain: HU_RETRAIN_MLXTUNE_TRAINER=mlx_lm, MODE=sft,
# corpus seth-sft-20260919) went through stock `mlx_lm.lora`. Its
# CompletionsDataset templates {prompt, completion} itself, without
# enable_thinking=False, and masks with an offset taken from the bare
# generation prompt. Measured on the GLM-4.5-Air tokenizer, one row trains:
#     \n <think> </think> \n <reply...> !
#   - no `/nothink` on the user turn (production always has it);
#   - `<think></think>` are TARGETS, though production puts them in the PROMPT;
#   - nothing marks the end of the turn: the last target is mlx_lm's pad id 0
#     ('!'), because default_loss's mask is `steps <= length` (one past the
#     last real token).
# This driver trains, per row, exactly
#     \n <reply...> <|user|>
#   - rows arrive pre-templated ({prompt, completion} from
#     chat_template_rows.format_completion_row); PretemplatedCompletionsDataset
#     tokenizes `prompt + completion` with plain encode() (GLM adds no BOS) and
#     masks every prompt token (offset = len(encode(prompt)));
#   - the loss mask ends at the last real token (turn_end_mask), so the
#     end-of-turn token is the LAST target and the pad is never trained.
# Everything else is stock mlx_lm.lora: same config keys, same optimizer, same
# iters/lr/rank/scale, same "Iter N: Val loss" lines nightly-retrain.sh reads.
#
# The hooks depend on mlx_lm internals read off 0.31.3 (create_dataset is
# resolved at call time inside mlx_lm.tuner.datasets; lora.py calls
# train_model/train by their module names; iterate_batches yields
# (offset, length) pairs). assert_mlx_lm_preconditions() refuses any other
# version rather than silently training without the fix.
#
# Modes:
#   --check-only --data DIR [--model ID --max-seq-length N]
#       tokenizer files only: build the dataset through the patched mlx_lm
#       loader and run the contract check. No weights. Used by the dry run.
#   -c CONFIG --adapter-path DIR [any mlx_lm.lora flag]
#       real training. Refuses unless HU_MLX_LM_ALLOW_LOAD=1 (set only by
#       scripts/train-glm-adapter.sh after it stopped prod and checked memory).
"""Templated mlx_lm.lora SFT for GLM persona adapters."""

import argparse
import functools
import inspect
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import chat_template_rows as ctr  # noqa: E402

PINNED_MLX_LM_VERSION = "0.31.3"
CONTRACT_LINE = "template contract holds on"


def is_glm_model(model_id) -> bool:
    return "glm" in str(model_id or "").lower()


class PretemplatedCompletionsDataset:
    """mlx_lm dataset protocol (__getitem__/__len__/process) over rows that are
    already rendered by chat_template_rows.format_completion_row. Returns
    (ids, offset) with offset = len(encode(prompt)): loss starts at the first
    reply token."""

    def __init__(self, data, tokenizer):
        self._data = data
        self.tokenizer = tokenizer

    def process(self, d):
        ids = list(self.tokenizer.encode(d["prompt"] + d["completion"]))
        return (ids, len(self.tokenizer.encode(d["prompt"])))

    def __getitem__(self, idx):
        return self._data[idx]

    def __len__(self):
        return len(self._data)


def create_dataset_for(data, tokenizer, config, model_id, original=None):
    """Route templated SFT rows to PretemplatedCompletionsDataset. Raw rows on a
    GLM base are refused (HU_MLX_LM_ALLOW_RAW_ROWS=1 overrides, loudly); any
    other base falls through to mlx_lm's own create_dataset."""
    rows = list(data)
    sft_rows = [r for r in rows if ctr.is_sft_row(r)]
    if rows and len(sft_rows) == len(rows) and all(ctr.is_templated_row(r) for r in rows):
        if not getattr(config, "mask_prompt", True):
            print("[mlx_lm_sft_templated] NOTE: mask_prompt=false in config ignored -- the "
                  "prompt (other people's messages) is never a training target here")
        return PretemplatedCompletionsDataset(rows, tokenizer)
    if is_glm_model(model_id):
        raw = sum(1 for r in rows if not ctr.is_templated_row(r))
        if os.environ.get("HU_MLX_LM_ALLOW_RAW_ROWS") == "1":
            print(f"[mlx_lm_sft_templated] WARNING: HU_MLX_LM_ALLOW_RAW_ROWS=1 -- {raw}/{len(rows)} "
                  "raw rows go through stock mlx_lm templating (no /nothink, no end-of-turn target)")
        else:
            sys.exit(f"[mlx_lm_sft_templated] FATAL: {raw}/{len(rows)} rows are not chat-templated "
                     "{prompt, completion} rows. Stock mlx_lm SFT on GLM trains `<think></think>` "
                     "as targets and no end-of-turn token. Build the corpus with "
                     "scripts/chat_template_rows.py (train-glm-adapter.sh does this).")
    if original is None:
        from mlx_lm.tuner import datasets as mlx_datasets
        original = mlx_datasets.create_dataset
    return original(rows, tokenizer, config)


def turn_end_mask(width, lengths):
    """mlx_lm default_loss's mask with the upper bound fixed: targets at
    positions [offset, length-1]. Stock `steps <= length` also trains the pad
    token one past the end (id 0, '!' on GLM)."""
    import mlx.core as mx
    steps = mx.arange(1, width + 1)
    return mx.logical_and(steps >= lengths[:, 0:1], steps < lengths[:, 1:])


def turn_end_loss(model, batch, lengths):
    """mlx_lm.tuner.trainer.default_loss with turn_end_mask (0.31.3 body)."""
    import mlx.core as mx
    import mlx.nn as nn
    inputs, targets = batch[:, :-1], batch[:, 1:]
    logits = model(inputs)
    mask = turn_end_mask(targets.shape[1], lengths)
    ce = nn.losses.cross_entropy(logits, targets) * mask
    ntoks = mask.sum()
    return ce.astype(mx.float32).sum() / ntoks, ntoks


def assert_sft_trainer_sees_template(dataset, tokenizer, max_seq_length, max_check=None):
    """Prove mlx_lm will train what production serves, through the dataset's
    OWN process() (what CacheDataset calls): exactly one leading [gMASK], the
    last token is the end-of-turn id, the sequence fits max_seq_length (mlx_lm
    truncates from the right, which would cut that token), and the mask offset
    is exactly the prompt length. Exits on the first violation."""
    gmask = tokenizer.convert_tokens_to_ids("[gMASK]")
    eot = tokenizer.convert_tokens_to_ids(ctr.END_OF_TURN)
    n = len(dataset) if max_check is None else min(len(dataset), max_check)
    for i in range(n):
        row = dataset[i]
        ids, offset = dataset.process(row)
        if not ids or ids[0] != gmask or list(ids).count(gmask) != 1:
            sys.exit(f"[mlx_lm_sft_templated] FATAL: row {i}: tokenization does not start with "
                     "exactly one [gMASK] -- BOS re-added or prompt mangled")
        if ids[-1] != eot:
            sys.exit(f"[mlx_lm_sft_templated] FATAL: row {i}: last token {ids[-1]} is not "
                     f"end-of-turn {eot} ({ctr.END_OF_TURN!r}) -- row untemplated")
        if len(ids) > max_seq_length:
            sys.exit(f"[mlx_lm_sft_templated] FATAL: row {i}: {len(ids)} tokens > max_seq_length "
                     f"{max_seq_length} -- right-truncation would drop the end-of-turn token")
        if offset != len(tokenizer.encode(row["prompt"])) or offset >= len(ids):
            sys.exit(f"[mlx_lm_sft_templated] FATAL: row {i}: mask offset {offset} != prompt "
                     "length -- the first reply token would get no loss")
    print(f"[mlx_lm_sft_templated] {CONTRACT_LINE} {n} trainer-tokenized sequences "
          f"(one leading [gMASK], last token {ctr.END_OF_TURN!r}; loss starts at the first "
          "reply token and ends at the end-of-turn token)", flush=True)
    return n


def wrap_train_model(original):
    """mlx_lm.lora.train_model, preceded by the contract check on the exact
    train/valid sets it is about to train on."""
    @functools.wraps(original)
    def train_model(args, model, train_set, valid_set, *rest, **kw):
        for ds in (train_set, valid_set):
            if isinstance(ds, PretemplatedCompletionsDataset):
                assert_sft_trainer_sees_template(ds, ds.tokenizer, int(args.max_seq_length))
        return original(args, model, train_set, valid_set, *rest, **kw)
    return train_model


def assert_mlx_lm_preconditions(installed_version=None):
    """Refuse unless the hooks below are guaranteed to take effect."""
    import importlib.metadata
    from mlx_lm import lora
    from mlx_lm.tuner import datasets, trainer

    version = installed_version or importlib.metadata.version("mlx-lm")
    if version != PINNED_MLX_LM_VERSION:
        sys.exit(f"[mlx_lm_sft_templated] FATAL: mlx-lm {version} installed; the dataset/loss "
                 f"hooks were verified against {PINNED_MLX_LM_VERSION} only. Re-verify, then "
                 "bump PINNED_MLX_LM_VERSION.")
    checks = [
        ("create_dataset(" in inspect.getsource(datasets.load_local_dataset),
         "load_local_dataset no longer calls create_dataset"),
        ("zip(offsets, lengths)" in inspect.getsource(trainer.iterate_batches),
         "iterate_batches no longer yields (offset, length)"),
        ("train_model(" in inspect.getsource(lora.run) and "train(" in inspect.getsource(lora.train_model),
         "lora.run/train_model no longer call train_model/train by name"),
    ]
    for ok, why in checks:
        if not ok:
            sys.exit(f"[mlx_lm_sft_templated] FATAL: {why} -- the fix would silently not apply")
    return True


def install(model_id):
    """Patch mlx_lm in-process: templated dataset routing, the contract check
    before training, and the turn-end loss."""
    from mlx_lm import lora
    from mlx_lm.tuner import datasets, trainer

    assert_mlx_lm_preconditions()
    original_create = datasets.create_dataset
    datasets.create_dataset = lambda data, tokenizer, config: create_dataset_for(
        data, tokenizer, config, model_id, original=original_create)
    lora.train_model = wrap_train_model(lora.train_model)
    lora.train = functools.partial(trainer.train, loss=turn_end_loss)


def _config_value(config_path, key):
    import yaml
    with open(config_path) as f:
        return (yaml.safe_load(f) or {}).get(key)


def cmd_check_only(args):
    from types import SimpleNamespace

    from mlx_lm.tuner import datasets

    assert_mlx_lm_preconditions()
    tok = ctr.load_serving_tokenizer(args.model)
    original = datasets.create_dataset
    n = 0
    for name in ("train.jsonl", "valid.jsonl"):
        path = Path(args.data) / name
        if not path.is_file():
            continue
        import json
        rows = [json.loads(x) for x in path.read_text().splitlines() if x.strip()]
        ds = create_dataset_for(rows, tok, SimpleNamespace(mask_prompt=True), args.model,
                                original=original)
        if not isinstance(ds, PretemplatedCompletionsDataset):
            sys.exit(f"[mlx_lm_sft_templated] FATAL: {path} is not a templated SFT corpus")
        n += assert_sft_trainer_sees_template(ds, tok, args.max_seq_length)
    if n == 0:
        sys.exit(f"[mlx_lm_sft_templated] FATAL: no train.jsonl/valid.jsonl under {args.data}")
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--check-only" in argv:
        ap = argparse.ArgumentParser(description=__doc__)
        ap.add_argument("--check-only", action="store_true")
        ap.add_argument("--data", required=True)
        ap.add_argument("--model", default=ctr.DEFAULT_MODEL)
        ap.add_argument("--max-seq-length", type=int, default=2048)
        return cmd_check_only(ap.parse_args(argv))
    if os.environ.get("HU_MLX_LM_ALLOW_LOAD") != "1":
        sys.exit("[mlx_lm_sft_templated] REFUSING to load the base model: HU_MLX_LM_ALLOW_LOAD=1 "
                 "is not set. Run via scripts/train-glm-adapter.sh, which stops production, "
                 "waits for a full reap and checks memory first (never two 56 GB loaders).")
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("-c", "--config")
    known, _ = ap.parse_known_args(argv)
    model_id = _config_value(known.config, "model") if known.config else None
    install(model_id)
    from mlx_lm import lora
    sys.argv = ["mlx_lm.lora"] + argv
    lora.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
