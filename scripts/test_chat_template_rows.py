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
    p_ids = tok.encode(row["prompt"])
    assert t["chosen_ids"][: len(p_ids)] == p_ids
    s = SimPOTrainer._tokenize_pair(fake, row)
    assert s["chosen_ids"][-1] == eot and s["rejected_ids"][-1] == eot


def test_newline_is_its_own_token_after_think_close(tok):
    """The reply's first token is a standalone "\n" -- so it is exactly the
    token the shared-prefix path used to leave untrained."""
    row = ctr.format_pair(tok, ROW, system=SYSTEM)
    ids = tok.encode(row["prompt"] + row["chosen"])
    n = len(tok.encode(row["prompt"]))
    assert tok.convert_ids_to_tokens(ids[n - 1:n + 2]) == ["</think>", "\u010a", "yeah"]


@pytest.mark.parametrize("mode", ["orpo", "simpo"])
@pytest.mark.parametrize("rejected", [ROW["rejected"], "yeah no, can't make it", ""])
def test_loss_starts_at_first_reply_token(tok, mode, rejected):
    """C1: with batch_size 1 mlx-tune trains only ids[prompt_length:], and
    prompt_length was the common prefix of chosen/rejected -- which always
    swallowed the leading "\n" (and shared words like "yeah"). After
    pin_prompt_length it must equal len(encode(prompt)) exactly."""
    import mlx_tune_train as mt
    from mlx_tune.rl_trainers import ORPOTrainer, SimPOTrainer

    cls = ORPOTrainer if mode == "orpo" else SimPOTrainer
    attr = mt._PAIR_TOKENIZERS[mode]
    trainer = SimpleNamespace(tokenizer=tok, max_seq_length=2048)
    setattr(trainer, attr, lambda s: getattr(cls, attr)(trainer, s))
    assert mt.pin_prompt_length(trainer, mode, tok) is True
    row = ctr.format_pair(tok, {**ROW, "rejected": rejected}, system=SYSTEM)
    t = getattr(trainer, attr)(row)
    n = len(tok.encode(row["prompt"]))
    assert t["prompt_length"] == n
    # the first trained target is the reply's first token, "\n" (or the stop
    # token itself for an empty rejected reply)
    assert t["chosen_ids"][t["prompt_length"]] == tok.convert_tokens_to_ids("\u010a")


def test_raw_row_is_what_the_old_pipeline_trained(tok):
    """Pins the bug being fixed: mlx_tune on a RAW row trains no template and
    no stop token. If this ever fails, mlx_tune started templating itself and
    chat_template_rows would double-template -- revisit the fix."""
    from mlx_tune.rl_trainers import ORPOTrainer

    t = ORPOTrainer._tokenize_preference_pair(
        SimpleNamespace(tokenizer=tok, max_seq_length=2048), ROW)
    assert tok.convert_tokens_to_ids("[gMASK]") not in t["chosen_ids"]
    assert t["chosen_ids"][-1] not in (151329, 151336, 151338)


def _orpo_trainer(tok, max_seq_length=2048):
    from mlx_tune.rl_trainers import ORPOTrainer

    trainer = SimpleNamespace(tokenizer=tok, max_seq_length=max_seq_length)
    trainer._tokenize_preference_pair = lambda s: ORPOTrainer._tokenize_preference_pair(trainer, s)
    return trainer


def test_driver_contract_check_accepts_templated_and_rejects_raw(tok):
    import mlx_tune_train as mt

    good = [ctr.format_pair(tok, ROW, system=SYSTEM)]
    trainer = _orpo_trainer(tok)
    mt.pin_prompt_length(trainer, "orpo", tok)
    assert mt.assert_trainer_sees_template(trainer, good, "orpo", tok) == 2
    with pytest.raises(SystemExit):
        mt.assert_trainer_sees_template(trainer, [ROW], "orpo", tok)
    # Truncation that cuts the end-of-turn token is caught too.
    short = _orpo_trainer(tok, max_seq_length=20)
    mt.pin_prompt_length(short, "orpo", tok)
    with pytest.raises(SystemExit):
        mt.assert_trainer_sees_template(short, good, "orpo", tok)


def test_driver_contract_check_rejects_unpinned_prompt_length(tok):
    """Without pin_prompt_length the stock trainer's common-prefix
    prompt_length is caught before any training step."""
    import mlx_tune_train as mt

    good = [ctr.format_pair(tok, ROW, system=SYSTEM)]
    with pytest.raises(SystemExit) as e:
        mt.assert_trainer_sees_template(_orpo_trainer(tok), good, "orpo", tok)
    assert "prompt_length" in str(e.value)


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


def test_kept_fraction_gate_covers_valid_too():
    ok = {"kept": 95, "dropped": 5, "drop_reasons": {}}
    assert ctr.kept_fraction_failures({"train.jsonl": ok, "valid.jsonl": ok}, 0.9) == []
    bad_valid = {"kept": 8, "dropped": 2, "drop_reasons": {"x": 2}}
    fails = ctr.kept_fraction_failures({"train.jsonl": ok, "valid.jsonl": bad_valid}, 0.9)
    assert len(fails) == 1 and fails[0].startswith("valid.jsonl")
    assert ctr.kept_fraction_failures({"train.jsonl": {"kept": 0, "dropped": 0,
                                                        "drop_reasons": {}}}, 0.9)


def test_cli_default_threshold_fails_when_valid_rows_drop(tok, tmp_path, monkeypatch):
    monkeypatch.setattr(ctr, "load_serving_tokenizer", lambda model_id=None: tok)
    src = tmp_path / "raw"
    src.mkdir()
    (src / "train.jsonl").write_text("".join(json.dumps(ROW) + "\n" for _ in range(10)))
    (src / "valid.jsonl").write_text(json.dumps(ROW) + "\n"
                                     + json.dumps({**ROW, "chosen": "<think>x</think>y"}) + "\n")
    assert ctr.main(["--in-dir", str(src), "--out-dir", str(tmp_path / "o")]) == 1


def test_system_prompt_token_count_and_reply_share(tok, tmp_path):
    src = tmp_path / "raw"
    src.mkdir()
    (src / "train.jsonl").write_text(json.dumps(ROW) + "\n")
    bare = ctr.convert_dir(src, tmp_path / "a", tokenizer=tok)
    long_sys = "You are Seth. " * 200
    withsys = ctr.convert_dir(src, tmp_path / "b", tokenizer=tok, system=long_sys)
    assert bare["system_prompt_tokens"] == 0 and withsys["system_prompt_tokens"] > 500
    a, b = bare["files"]["train.jsonl"], withsys["files"]["train.jsonl"]
    # a long system prompt shrinks the reply's share of the NLL denominator
    assert b["chosen_reply_share_median"] < a["chosen_reply_share_median"] / 5


# --- the pin must provably take effect (mlx-tune internals it depends on) ---

def _native_trainer():
    return SimpleNamespace(use_native=True)


@pytest.mark.parametrize("mode", ["orpo", "simpo"])
def test_pin_preconditions_hold_on_installed_mlx_tune(mode):
    import importlib.metadata
    import mlx_tune_train as mt

    assert importlib.metadata.version("mlx-tune") == mt.PINNED_MLX_TUNE_VERSION == "0.6.0"
    assert mt.assert_pin_preconditions(_native_trainer(), mode) is True


def test_pin_preconditions_refuse_other_mlx_tune_version(monkeypatch):
    import importlib.metadata
    import mlx_tune_train as mt

    monkeypatch.setattr(importlib.metadata, "version", lambda name: "0.7.0")
    with pytest.raises(SystemExit) as e:
        mt.assert_pin_preconditions(_native_trainer(), "orpo")
    assert "0.7.0" in str(e.value) and "0.6.0" in str(e.value)


@pytest.mark.parametrize("mode,cls_name,loop", [("orpo", "ORPOTrainer", "_train_native"),
                                                ("simpo", "SimPOTrainer", "train")])
def test_pin_preconditions_refuse_renamed_call_site(monkeypatch, mode, cls_name, loop):
    """Simulates an mlx-tune whose loop calls a renamed tokenizer: the pin
    would shadow a method nobody calls."""
    import mlx_tune_train as mt
    from mlx_tune import rl_trainers

    def renamed(self):
        return self._tokenize_pair_v2(None)
    monkeypatch.setattr(getattr(rl_trainers, cls_name), loop, renamed)
    with pytest.raises(SystemExit) as e:
        mt.assert_pin_preconditions(_native_trainer(), mode)
    assert "no longer calls" in str(e.value)


def test_pin_preconditions_refuse_subprocess_fallback():
    import mlx_tune_train as mt

    with pytest.raises(SystemExit) as e:
        mt.assert_pin_preconditions(SimpleNamespace(use_native=False), "orpo")
    assert "use_native" in str(e.value)


def test_pin_preconditions_skip_kto():
    import mlx_tune_train as mt

    assert mt.assert_pin_preconditions(SimpleNamespace(use_native=False), "kto") is False


@pytest.mark.parametrize("batch_size,expect", [(1, "first reply token"),
                                               (4, "non-shared path")])
def test_contract_log_line_matches_batch_size(tok, capsys, batch_size, expect):
    import mlx_tune_train as mt

    trainer = _orpo_trainer(tok)
    mt.pin_prompt_length(trainer, "orpo", tok)
    mt.assert_trainer_sees_template(trainer, [ctr.format_pair(tok, ROW, system=SYSTEM)],
                                    "orpo", tok, batch_size=batch_size)
    out = capsys.readouterr().out
    assert expect in out
    if batch_size != 1:
        assert "first reply token" not in out
