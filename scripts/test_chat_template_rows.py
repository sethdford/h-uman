#!/usr/bin/env python3
# scripts/test_chat_template_rows.py
#
# Model-free contract tests for scripts/chat_template_rows.py: a training row
# must reach the trainer as the exact token stream production serves.
#
# Uses the REAL local GLM-4.5-Air-4bit tokenizer (tokenizer.json,
# tokenizer_config.json, chat_template.jinja from the HF cache; never a
# *.safetensors file, never the network) and the REAL production render
# function, imported from gemma-realtime-1/scripts/mlx-server.py
# (prepare_prompt_lm). Importing mlx-server.py is side-effect free: the model is
# None at module scope and mlx_lm/mlx_vlm load lazily (same pattern as
# gemma-realtime-1/scripts/test_skip_thinking_primer.py). Nothing here binds a
# port or starts a server.
#
# Run: ~/.human/venvs/mlxtune312/bin/python -m pytest scripts/test_chat_template_rows.py -v
import importlib.util
import json
import os
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

os.environ.setdefault("HF_HUB_OFFLINE", "1")
sys.path.insert(0, str(Path(__file__).parent))
import chat_template_rows as ctr  # noqa: E402

MODEL = ctr.DEFAULT_MODEL
MLX_SERVER = Path(os.environ.get(
    "HU_MLX_SERVER_PY", Path.home() / "Documents/gemma-realtime-1/scripts/mlx-server.py"))

ROW = {
    "prompt": "Them: are you coming saturday?\nSeth: probably\nThem: ok lmk by friday",
    "chosen": "yeah I'll be there",
    "rejected": "Absolutely! I would be delighted to attend on Saturday. Let me know what to bring.",
}
SYSTEM = "You are Seth. Reply as Seth would, in one short text."


@pytest.fixture(scope="module")
def tok():
    try:
        return ctr.load_serving_tokenizer(MODEL)
    except Exception as e:  # tokenizer files not in the local HF cache
        pytest.skip(f"local {MODEL} tokenizer unavailable: {e}")


@pytest.fixture(scope="module")
def srv(tok):
    if not MLX_SERVER.is_file():
        pytest.skip(f"production server source not found: {MLX_SERVER}")
    spec = importlib.util.spec_from_file_location("mlx_server_under_test", MLX_SERVER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    # Production state for the GLM base: mlx_lm's TokenizerWrapper as `processor`,
    # GEMMA_DISABLE_THINKING=1 from ai.human.mlx-server.plist.
    mod.processor = tok
    mod.model_id = MODEL
    return mod


@pytest.fixture()
def prod_env(monkeypatch):
    monkeypatch.setenv("GEMMA_DISABLE_THINKING", "1")
    monkeypatch.delenv("HU_NO_THINK_ANY_MODEL", raising=False)


# --------------------------------------------------------------------------
# The three required contracts
# --------------------------------------------------------------------------


def test_prompt_suffix_equals_production_suffix(tok):
    row = ctr.format_pair(tok, ROW, system=SYSTEM)
    assert row["prompt"].endswith("<|assistant|>\n<think></think>")
    assert row["prompt"].endswith(ctr.PRODUCTION_SUFFIX)
    assert row["prompt"].startswith("[gMASK]<sop><|system|>\n" + SYSTEM)
    # enable_thinking=False also marks the user turn, exactly as in production.
    assert "ok lmk by friday/nothink<|assistant|>" in row["prompt"]


def test_chosen_and_rejected_end_with_stop_token(tok):
    row = ctr.format_pair(tok, ROW, system=SYSTEM)
    assert row["chosen"] == "\nyeah I'll be there<|user|>"
    assert row["rejected"].startswith("\nAbsolutely!") and row["rejected"].endswith("<|user|>")
    # ...and that text is the single end-of-turn TOKEN the model emits, which
    # mlx_lm treats as a generation stop (config.json eos_token_id list).
    ids = tok.encode(row["prompt"] + row["chosen"])
    eot = tok.convert_tokens_to_ids("<|user|>")
    assert ids[-1] == eot == 151336
    snap = Path(tok.name_or_path) if Path(tok.name_or_path).is_dir() else None
    if snap is None:
        from huggingface_hub import try_to_load_from_cache
        snap = Path(try_to_load_from_cache(MODEL, "config.json")).parent
    assert eot in json.loads((snap / "config.json").read_text())["eos_token_id"]


def test_template_matches_mlx_server_render(tok, srv, prod_env):
    """Same messages -> byte-identical prompt to production's prepare_prompt_lm."""
    for messages in (
        [{"role": "system", "content": SYSTEM}, {"role": "user", "content": ROW["prompt"]}],
        [{"role": "user", "content": ROW["prompt"]}],
        [{"role": "system", "content": SYSTEM},
         {"role": "user", "content": "you up?"},
         {"role": "assistant", "content": "yeah"},
         {"role": "user", "content": "classify this as yes or no: did he agree?"}],
    ):
        ours = ctr.render_prompt(tok, messages)
        theirs = srv.prepare_prompt_lm(messages)
        assert ours == theirs
        assert theirs.endswith(ctr.PRODUCTION_SUFFIX)


# --------------------------------------------------------------------------
# What the trainer actually sees (mlx_tune's own tokenization methods)
# --------------------------------------------------------------------------


def test_mlx_tune_orpo_tokenization_of_templated_row(tok):
    from mlx_tune.rl_trainers import ORPOTrainer, SimPOTrainer

    row = ctr.format_pair(tok, ROW, system=SYSTEM)
    fake = SimpleNamespace(tokenizer=tok, max_seq_length=2048)
    gmask, eot = tok.convert_tokens_to_ids("[gMASK]"), tok.convert_tokens_to_ids("<|user|>")
    t = ORPOTrainer._tokenize_preference_pair(fake, row)
    for ids in (t["chosen_ids"], t["rejected_ids"]):
        assert ids[0] == gmask and ids.count(gmask) == 1   # encode() re-added no BOS
        assert ids[-1] == eot                              # the turn end is trained
    # The shared prefix covers the whole production prompt.
    p_ids = tok.encode(row["prompt"])
    assert t["prompt_length"] >= len(p_ids)
    assert t["chosen_ids"][: len(p_ids)] == p_ids
    s = SimPOTrainer._tokenize_pair(fake, row)
    assert s["chosen_ids"][-1] == eot and s["rejected_ids"][-1] == eot


def test_raw_row_is_what_the_old_pipeline_trained(tok):
    """Pins the bug being fixed: mlx_tune on a RAW row trains no template and
    no stop token. If this ever fails, mlx_tune started templating itself and
    chat_template_rows would double-template -- revisit the fix."""
    from mlx_tune.rl_trainers import ORPOTrainer

    t = ORPOTrainer._tokenize_preference_pair(
        SimpleNamespace(tokenizer=tok, max_seq_length=2048), ROW)
    assert tok.convert_tokens_to_ids("[gMASK]") not in t["chosen_ids"]
    assert t["chosen_ids"][-1] not in (151329, 151336, 151338)


def test_driver_contract_check_accepts_templated_and_rejects_raw(tok):
    import mlx_tune_train as mt
    from mlx_tune.rl_trainers import ORPOTrainer

    trainer = SimpleNamespace(tokenizer=tok, max_seq_length=2048)
    trainer._tokenize_preference_pair = lambda s: ORPOTrainer._tokenize_preference_pair(trainer, s)
    good = [ctr.format_pair(tok, ROW, system=SYSTEM)]
    assert mt.assert_trainer_sees_template(trainer, good, "orpo", tok) == 1
    with pytest.raises(SystemExit):
        mt.assert_trainer_sees_template(trainer, [ROW], "orpo", tok)
    # Truncation that cuts the end-of-turn token is caught too.
    trainer.max_seq_length = 20
    with pytest.raises(SystemExit):
        mt.assert_trainer_sees_template(trainer, good, "orpo", tok)


# --------------------------------------------------------------------------
# Rows that must not be trained on
# --------------------------------------------------------------------------


@pytest.mark.parametrize("bad", [
    {**ROW, "chosen": "<think>should I go</think>yeah"},          # harvested scaffold
    {**ROW, "chosen": "ok<|user|>"},                               # already templated
    {**ROW, "prompt": "[gMASK]<sop><|user|>\nhi<|assistant|>"},    # templated twice
    {**ROW, "chosen": "   "},                                      # teaches empty reply
    {**ROW, "chosen": ROW["rejected"]},                            # no preference
])
def test_bad_rows_are_rejected(tok, bad):
    with pytest.raises(ctr.RowRejected):
        ctr.format_pair(tok, bad, system=SYSTEM)


def test_empty_rejected_is_kept_as_an_immediate_end_of_turn(tok):
    row = ctr.format_pair(tok, {**ROW, "rejected": ""}, system=SYSTEM)
    assert row["rejected"] == "<|user|>"


def test_overlong_row_is_dropped_not_truncated(tok):
    row = ctr.format_pair(tok, ROW, system=SYSTEM)
    with pytest.raises(ctr.RowRejected) as e:
        ctr.check_trainer_tokenization(tok, row, max_seq_length=10)
    assert "max_seq_length" in e.value.key


def test_convert_dir_writes_manifest_and_templated_rows(tok, tmp_path):
    src = tmp_path / "raw"
    src.mkdir()
    rows = [ROW, {**ROW, "chosen": "<think>x</think>y"}, {**ROW, "weight": 8}]
    (src / "train.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
    (src / "valid.jsonl").write_text(json.dumps(ROW) + "\n")
    m = ctr.convert_dir(src, tmp_path / "out", model_id=MODEL, tokenizer=tok)
    assert m["files"]["train.jsonl"]["kept"] == 2
    assert m["files"]["train.jsonl"]["drop_reasons"] == {
        "reply contains template scaffold '<think>'": 1}
    assert m["end_of_turn_id"] == 151336 and m["prompt_suffix"] == ctr.PRODUCTION_SUFFIX
    out = [json.loads(line) for line in (tmp_path / "out/train.jsonl").read_text().splitlines()]
    assert all(ctr.is_templated_row(r) for r in out)
    assert out[1]["weight"] == 8                                   # metadata carried
    assert ctr.templated_report(tmp_path / "out/train.jsonl") == {
        "count": 2, "templated": 2, "raw": 0}
    assert ctr.templated_report(src / "train.jsonl")["raw"] == 3


def test_driver_refuses_raw_glm_corpus(tmp_path, monkeypatch):
    import mlx_tune_train as mt

    (tmp_path / "train.jsonl").write_text(json.dumps(ROW) + "\n")
    monkeypatch.delenv("HU_MLX_TUNE_ALLOW_RAW_ROWS", raising=False)
    with pytest.raises(SystemExit):
        mt.require_templated_corpus(MODEL, tmp_path)
    # non-GLM bases are out of this contract's scope
    assert mt.require_templated_corpus("mlx-community/gemma-4-31b-it-4bit", tmp_path)["raw"] == 1
