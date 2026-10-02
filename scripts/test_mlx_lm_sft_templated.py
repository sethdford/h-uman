#!/usr/bin/env python3
# scripts/test_mlx_lm_sft_templated.py
#
# Contract tests for the nightly SFT path (scripts/mlx_lm_sft_templated.py +
# the {prompt, completion} half of scripts/chat_template_rows.py).
#
# The nightly retrain (ai.human.nightly-retrain, HU_RETRAIN_MLXTUNE_TRAINER=
# mlx_lm, MODE=sft) trained through stock `mlx_lm.lora` on raw
# {prompt, completion} rows. Measured 2026-10-02 on the GLM-4.5-Air tokenizer,
# that path trains these targets for one row:
#     \n <think> </think> \n <reply...> !
# i.e. no `/nothink` on the user turn, the model is taught to EMIT the
# `<think></think>` that production already puts in the prompt, and the turn
# "ends" on mlx_lm's pad id 0 ('!') -- never on a stop token. These tests pin
# that gap and prove the fixed path trains exactly
#     \n <reply...> <|user|>
# through mlx_lm's OWN dataset/batching/mask code, not a re-implementation.
#
# Uses the REAL local GLM-4.5-Air-4bit tokenizer files (no weights, no
# network, no server). Skips if they are not cached.
# Run: ~/.human/venvs/mlxtune312/bin/python -m pytest scripts/test_mlx_lm_sft_templated.py -v
import json
import os
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

os.environ.setdefault("HF_HUB_OFFLINE", "1")
sys.path.insert(0, str(Path(__file__).parent))
import chat_template_rows as ctr  # noqa: E402
import mlx_lm_sft_templated as sft  # noqa: E402

MODEL = ctr.DEFAULT_MODEL
EOT_ID = 151336
SFT_ROW = {"prompt": "Seth: You too\nThem: Headache go away?",
           "completion": "Yeah feeling pretty good today"}


@pytest.fixture(scope="module")
def tok():
    try:
        return ctr.load_serving_tokenizer(MODEL)
    except Exception as e:  # tokenizer files not in the local HF cache
        pytest.skip(f"local {MODEL} tokenizer unavailable: {e}")


def _trained_targets(tok, dataset, loss_mask):
    """Token strings mlx_lm will put loss on for dataset[0], using mlx_lm's own
    iterate_batches and the given mask function (default_loss's formula, or
    ours)."""
    import mlx.core as mx
    from mlx_lm.tuner.datasets import CacheDataset
    from mlx_lm.tuner.trainer import iterate_batches

    batch, lengths = next(iterate_batches(CacheDataset(dataset), 1, 2048))
    mask = loss_mask(batch[:, 1:].shape[1], lengths)
    targets = batch[:, 1:][0].tolist()
    return tok.convert_ids_to_tokens([int(t) for t, m in zip(targets, mask[0].tolist()) if m])


def _stock_mask(width, lengths):  # mlx_lm.tuner.trainer.default_loss, verbatim
    import mlx.core as mx
    steps = mx.arange(1, width + 1)
    return mx.logical_and(steps >= lengths[:, 0:1], steps <= lengths[:, 1:])


# --------------------------------------------------------------------------
# The gap being fixed (pinned, so an mlx_lm change that alters it is noticed)
# --------------------------------------------------------------------------


def test_stock_mlx_lm_sft_trains_no_stop_token_and_emits_think_tags(tok):
    from mlx_lm.tuner.datasets import CompletionsDataset

    ds = CompletionsDataset([SFT_ROW], tok, "prompt", "completion", True)
    got = _trained_targets(tok, ds, _stock_mask)
    assert got[:4] == ["Ċ", "<think>", "</think>", "Ċ"]   # think tags are TARGETS
    assert got[-1] == "!"                                             # pad id 0, not a stop
    assert "<|user|>" not in got and "<|endoftext|>" not in got
    ids, _ = ds.process(SFT_ROW)
    assert "/nothink" not in tok.decode(ids)


# --------------------------------------------------------------------------
# Row rendering ({prompt, completion} -> production template + stop token)
# --------------------------------------------------------------------------


def test_sft_row_renders_production_prompt_and_stop_token(tok):
    row = ctr.format_completion_row(tok, SFT_ROW)
    assert row["prompt"].startswith(ctr.PRODUCTION_PREFIX)
    assert row["prompt"].endswith("Headache go away?/nothink" + ctr.PRODUCTION_SUFFIX)
    assert row["completion"] == "\nYeah feeling pretty good today<|user|>"
    assert ctr.is_templated_row(row) and not ctr.is_templated_row(SFT_ROW)


@pytest.mark.parametrize("bad", [
    {**SFT_ROW, "completion": "  "},
    {**SFT_ROW, "completion": "<think>x</think>y"},
    {**SFT_ROW, "prompt": "[gMASK]<sop><|user|>\nhi"},
])
def test_bad_sft_rows_are_rejected(tok, bad):
    with pytest.raises(ctr.RowRejected):
        ctr.format_completion_row(tok, bad)


def test_convert_dir_handles_sft_corpus(tok, tmp_path):
    src = tmp_path / "raw"
    src.mkdir()
    rows = [SFT_ROW, {**SFT_ROW, "completion": ""}, {"prompt": "Them: yo", "completion": "sup"}]
    (src / "train.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
    (src / "valid.jsonl").write_text(json.dumps(SFT_ROW) + "\n")
    m = ctr.convert_dir(src, tmp_path / "out", tokenizer=tok, max_seq_length=1024)
    assert m["files"]["train.jsonl"]["kept"] == 2
    assert m["files"]["train.jsonl"]["drop_reasons"] == {
        "empty reply would teach an immediate end-of-turn": 1}
    assert ctr.templated_report(tmp_path / "out/train.jsonl") == {
        "count": 2, "templated": 2, "raw": 0}
    out = [json.loads(x) for x in (tmp_path / "out/train.jsonl").read_text().splitlines()]
    assert set(out[0]) == {"prompt", "completion"}


# --------------------------------------------------------------------------
# What mlx_lm actually trains on the templated rows
# --------------------------------------------------------------------------


def test_templated_rows_route_to_pretemplated_dataset(tok):
    row = ctr.format_completion_row(tok, SFT_ROW)
    ds = sft.create_dataset_for([row], tok, SimpleNamespace(mask_prompt=True), MODEL)
    assert isinstance(ds, sft.PretemplatedCompletionsDataset)
    ids, offset = ds.process(ds[0])
    assert ids[0] == tok.convert_tokens_to_ids("[gMASK]") and ids.count(ids[0]) == 1
    assert ids[-1] == EOT_ID
    assert offset == len(tok.encode(row["prompt"]))


def test_fixed_path_trains_reply_then_stop_token_only(tok):
    """The headline contract: loss on exactly `\\n <reply> <|user|>`."""
    row = ctr.format_completion_row(tok, SFT_ROW)
    ds = sft.create_dataset_for([row], tok, SimpleNamespace(mask_prompt=True), MODEL)
    got = _trained_targets(tok, ds, sft.turn_end_mask)
    assert got[0] == "Ċ" and got[1] == "Yeah"     # first reply token is trained
    assert got[-1] == "<|user|>"                        # the turn end is the LAST target
    assert "<think>" not in got and "</think>" not in got and "!" not in got
    # stock default_loss would still train mlx_lm's pad after the stop token
    assert _trained_targets(tok, ds, _stock_mask)[-2:] == ["<|user|>", "!"]


def test_turn_end_loss_matches_default_loss_except_the_pad_target():
    import mlx.core as mx
    lengths = mx.array([[3, 7]])
    stock, ours = _stock_mask(10, lengths)[0].tolist(), sft.turn_end_mask(10, lengths)[0].tolist()
    assert [i for i, (a, b) in enumerate(zip(stock, ours)) if a != b] == [6]   # step 7 = pad


def test_raw_rows_on_glm_are_refused(tok, monkeypatch):
    monkeypatch.delenv("HU_MLX_LM_ALLOW_RAW_ROWS", raising=False)
    with pytest.raises(SystemExit):
        sft.create_dataset_for([SFT_ROW], tok, SimpleNamespace(mask_prompt=True), MODEL)


def test_contract_check_accepts_templated_rejects_overlong(tok, capsys):
    row = ctr.format_completion_row(tok, SFT_ROW)
    ds = sft.PretemplatedCompletionsDataset([row, row], tok)
    assert sft.assert_sft_trainer_sees_template(ds, tok, max_seq_length=1024) == 2
    assert "template contract holds on 2 trainer-tokenized sequences" in capsys.readouterr().out
    with pytest.raises(SystemExit) as e:
        sft.assert_sft_trainer_sees_template(ds, tok, max_seq_length=10)
    assert "max_seq_length" in str(e.value)


def test_contract_check_rejects_a_row_without_stop_token(tok):
    row = ctr.format_completion_row(tok, SFT_ROW)
    broken = {**row, "completion": row["completion"][: -len(ctr.END_OF_TURN)]}
    with pytest.raises(SystemExit) as e:
        sft.assert_sft_trainer_sees_template(sft.PretemplatedCompletionsDataset([broken], tok),
                                             tok, max_seq_length=1024)
    assert "end-of-turn" in str(e.value)


def test_train_model_wrapper_checks_contract_before_training(tok):
    """The check is wired into mlx_lm.lora's own train_model entry: a bad set
    exits BEFORE the original train_model runs."""
    calls = []
    wrapped = sft.wrap_train_model(lambda *a, **k: calls.append(a))
    row = ctr.format_completion_row(tok, SFT_ROW)
    good = sft.PretemplatedCompletionsDataset([row], tok)
    args = SimpleNamespace(max_seq_length=1024)
    wrapped(args, None, good, good)
    assert len(calls) == 1
    bad = sft.PretemplatedCompletionsDataset(
        [{**row, "completion": row["completion"][:-len(ctr.END_OF_TURN)]}], tok)
    with pytest.raises(SystemExit):
        wrapped(args, None, bad, good)
    assert len(calls) == 1


def test_pin_preconditions_hold_on_installed_mlx_lm():
    assert sft.assert_mlx_lm_preconditions() is True


def test_pin_preconditions_refuse_other_mlx_lm_version(monkeypatch):
    import importlib.metadata
    monkeypatch.setattr(importlib.metadata, "version", lambda name: "0.99.0")
    with pytest.raises(SystemExit) as e:
        sft.assert_mlx_lm_preconditions()
    assert "0.99.0" in str(e.value)


def test_check_only_cli_on_templated_and_raw_dirs(tok, tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(ctr, "load_serving_tokenizer", lambda model_id=None: tok)
    raw = tmp_path / "raw"
    raw.mkdir()
    (raw / "train.jsonl").write_text(json.dumps(SFT_ROW) + "\n")
    (raw / "valid.jsonl").write_text(json.dumps(SFT_ROW) + "\n")
    ctr.convert_dir(raw, tmp_path / "tmpl", tokenizer=tok, max_seq_length=1024)
    assert sft.main(["--check-only", "--data", str(tmp_path / "tmpl"),
                     "--model", MODEL, "--max-seq-length", "1024"]) == 0
    # one contract line per split (train + valid), each on the real loader path
    assert capsys.readouterr().out.count("template contract holds on 1 ") == 2
    monkeypatch.delenv("HU_MLX_LM_ALLOW_RAW_ROWS", raising=False)
    with pytest.raises(SystemExit):
        sft.main(["--check-only", "--data", str(raw), "--model", MODEL])


def test_train_refuses_without_allow_load(tmp_path, monkeypatch):
    monkeypatch.delenv("HU_MLX_LM_ALLOW_LOAD", raising=False)
    with pytest.raises(SystemExit) as e:
        sft.main(["-c", str(tmp_path / "config.yaml"), "--adapter-path", str(tmp_path / "a")])
    assert "HU_MLX_LM_ALLOW_LOAD" in str(e.value)
