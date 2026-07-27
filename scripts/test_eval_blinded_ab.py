#!/usr/bin/env python3
"""Unit tests for eval_blinded_ab.py harness correctness (stdlib runner).

Both suites pin bugs found 2026-07-27 while investigating why the proxy
fool rate read 20%:

1. The results JSON recorded "mode": "cli" for a --mlx run, because the
   expression only ever tested USE_GATEWAY. Every consumer of
   data/eval_blinded_ab.json — including a full session of analysis — was
   told the C pipeline generated replies that raw MLX generated.

2. get_ai_response_mlx(message) took no conversation history at all. The
   docstring on the CLI variant explains that omitting the thread makes the
   measurement meaningless (the human reply it is scored against WAS written
   with the thread visible), but the nightly runs --mlx, so the fix never
   applied to the path in use. 7/45 replies in the 07-27 run begged for
   context Seth plainly had.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval_blinded_ab as E


# ---- 1. mode label ---------------------------------------------------------

def test_mode_mlx_is_not_labelled_cli():
    assert E.run_mode(use_gateway=False, use_mlx=True) == "mlx"


def test_mode_cli_when_no_flags():
    assert E.run_mode(use_gateway=False, use_mlx=False) == "cli"


def test_mode_gateway():
    assert E.run_mode(use_gateway=True, use_mlx=False) == "gateway"


def test_mode_gateway_wins_over_mlx():
    """Must mirror get_ai_response()'s precedence: gateway is checked first."""
    assert E.run_mode(use_gateway=True, use_mlx=True) == "gateway"


# ---- 2. MLX conversation history ------------------------------------------

def test_mlx_messages_without_context_is_system_plus_user():
    m = E.build_mlx_messages("SYS", "hey", None)
    assert [x["role"] for x in m] == ["system", "user"]
    assert m[0]["content"] == "SYS"
    assert m[-1]["content"] == "hey"


def test_mlx_messages_include_thread_in_order():
    turns = [
        {"from": "them", "text": "you around?"},
        {"from": "seth", "text": "yeah whats up"},
        {"from": "them", "text": "dinner at 7?"},
    ]
    m = E.build_mlx_messages("SYS", "still good?", turns)
    assert [x["role"] for x in m] == [
        "system", "user", "assistant", "user", "user"
    ]
    assert [x["content"] for x in m[1:4]] == [
        "you around?", "yeah whats up", "dinner at 7?"
    ]
    assert m[-1]["content"] == "still good?"


def test_mlx_messages_map_seth_to_assistant():
    m = E.build_mlx_messages("SYS", "x", [{"from": "seth", "text": "mine"}])
    assert m[1] == {"role": "assistant", "content": "mine"}


def test_mlx_messages_unknown_sender_is_user():
    m = E.build_mlx_messages("SYS", "x", [{"from": "whoever", "text": "theirs"}])
    assert m[1]["role"] == "user"


def test_mlx_messages_skip_blank_turns():
    turns = [{"from": "them", "text": "   "}, {"from": "them", "text": ""},
             {"from": "them", "text": "real"}]
    m = E.build_mlx_messages("SYS", "x", turns)
    assert [x["content"] for x in m] == ["SYS", "real", "x"]


def test_mlx_messages_tolerates_malformed_turn():
    """Ground truth is machine-extracted; a None or key-less turn must not
    abort a 45-trial run."""
    turns = [None, {}, {"text": "no from key"}, {"from": "them"}]
    m = E.build_mlx_messages("SYS", "x", turns)
    assert [x["content"] for x in m] == ["SYS", "no from key", "x"]


# ---- 3. judge thinking budget ---------------------------------------------
# gemini-3.x shares maxOutputTokens between invisible thinking and the visible
# reply (CLAUDE.md). Unset, a judgment that thinks hard leaves too few tokens
# for the JSON body -> "Unterminated string" -> the trial is silently dropped.
# 18/50 trials were lost this way on 2026-07-27, and the loss is biased: the
# judge thinks longest on the closest calls.

def test_judge_config_sets_thinking_budget_explicitly():
    cfg = E.judge_gen_config(0.2, E._BLINDED_AB_JUDGE_SCHEMA)
    assert "thinkingConfig" in cfg, "gemini-3.x needs an explicit thinking budget"
    assert isinstance(cfg["thinkingConfig"].get("thinkingBudget"), int)


def test_judge_config_leaves_room_for_the_json_body():
    cfg = E.judge_gen_config(0.2, E._BLINDED_AB_JUDGE_SCHEMA)
    budget = cfg["thinkingConfig"]["thinkingBudget"]
    assert cfg["maxOutputTokens"] - budget >= 2048, (
        "thinking must not be able to starve the response body")


def test_judge_config_still_carries_schema_and_temperature():
    cfg = E.judge_gen_config(0.2, E._BLINDED_AB_JUDGE_SCHEMA)
    assert cfg["temperature"] == 0.2
    assert cfg["responseSchema"] is E._BLINDED_AB_JUDGE_SCHEMA
    assert cfg["responseMimeType"] == "application/json"


def test_judge_config_without_schema_omits_schema_keys():
    cfg = E.judge_gen_config(0.7, None)
    assert "responseSchema" not in cfg and "responseMimeType" not in cfg
    assert "thinkingConfig" in cfg


# ---- 4. gateway (product) path --------------------------------------------
# The --mlx path scores a hand-written SETH_SYSTEM_PROMPT against raw MLX, so
# the persona pipeline is never exercised and every style claim in that prompt
# becomes an AI tell the moment it is wrong ("Lowercase." was ~10x off, and
# "Abbreviate (gonna, tbh...)" drove tbh 200x). The gateway path runs the real
# agent turn, which supplies the persona itself.

def test_gateway_messages_carry_no_system_prompt():
    """The product owns the persona. A harness-authored system prompt here
    would reintroduce exactly the artifact class --gateway exists to remove."""
    m = E.build_gateway_messages("hey", None)
    assert all(x["role"] != "system" for x in m), (
        "the gateway must not be fed a harness-authored persona")


def test_gateway_messages_minimal_is_single_user_turn():
    assert E.build_gateway_messages("hey", None) == [
        {"role": "user", "content": "hey"}]


def test_gateway_messages_include_thread_in_order():
    turns = [{"from": "them", "text": "you around?"},
             {"from": "seth", "text": "yeah whats up"}]
    m = E.build_gateway_messages("dinner at 7?", turns)
    assert [x["role"] for x in m] == ["user", "assistant", "user"]
    assert m[-1]["content"] == "dinner at 7?"


def test_gateway_messages_skip_blank_and_malformed():
    turns = [None, {}, {"from": "them", "text": "  "}, {"from": "them", "text": "ok"}]
    m = E.build_gateway_messages("x", turns)
    assert [x["content"] for x in m] == ["ok", "x"]


def test_gateway_url_prefers_env_override():
    assert E.gateway_url_from_config({"gateway": {"port": 3006}},
                                     env_url="http://host:9999") == "http://host:9999"


def test_gateway_url_reads_configured_port():
    """Default was hardcoded :3002 while the daemon listens on the configured
    port (3006 here) — --gateway would have failed connection-refused."""
    assert E.gateway_url_from_config({"gateway": {"port": 3006}}) == "http://127.0.0.1:3006"


def test_gateway_url_falls_back_when_config_unusable():
    for bad in (None, {}, {"gateway": {}}, {"gateway": None}):
        assert E.gateway_url_from_config(bad).endswith(":3002")


# ---- runner ----------------------------------------------------------------

def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for fn in fns:
        try:
            fn()
            print(f"PASS {fn.__name__}")
        except Exception as e:
            failed += 1
            print(f"FAIL {fn.__name__}: {e}")
    print(f"\n{len(fns) - failed}/{len(fns)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    _run()
