#!/usr/bin/env python3
"""Unit tests for scripts/ab_phrase_banks.py (stdlib runner — no pytest dep).

The harness answers one question: do the all-lowercase mined phrase banks and
humor examples drive the product's 86%-lowercase output, against Seth's
measured 14%? These tests cover the pure parts — sandbox construction inputs,
sampling determinism, and the style measurement — so the expensive live half
(two isolated gateways, ~30 model turns) only ever runs on logic that is
already pinned.

The sandbox purity tests matter more than they look: this harness exists to
A/B a LIVE-state input (~/.human/phrase_banks.json, ~/.human/personas/), and
the whole design premise is that it never mutates the real files. A helper
that mutated its input dict would be one refactor away from writing through.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab_phrase_banks as A


# ---- sandbox config --------------------------------------------------------

def test_sandbox_config_moves_the_gateway_port():
    """The isolated instance must not bind the live daemon's port."""
    out = A.sandbox_config({"gateway": {"port": 3006}}, 3011)
    assert out["gateway"]["port"] == 3011


def test_sandbox_config_preserves_unrelated_settings():
    out = A.sandbox_config({"gateway": {"port": 3006, "host": "127.0.0.1"},
                            "personalization": {"lora_adapter_id": "v5"}}, 3011)
    assert out["gateway"]["host"] == "127.0.0.1"
    assert out["personalization"]["lora_adapter_id"] == "v5"


def test_sandbox_config_does_not_mutate_the_caller_copy():
    """Guard-rail: the source dict is read from the REAL ~/.human/config.json."""
    src = {"gateway": {"port": 3006}}
    A.sandbox_config(src, 3011)
    assert src["gateway"]["port"] == 3006


def test_sandbox_config_handles_missing_gateway_block():
    assert A.sandbox_config({}, 3011)["gateway"]["port"] == 3011


# ---- the two A/B variables -------------------------------------------------

def test_emptied_phrase_banks_has_every_bank_empty():
    banks = A.emptied_phrase_banks()["imessage"]
    assert set(banks) == {"fillers", "starters", "backchannels", "farewells"}
    assert all(v == [] for v in banks.values())


def test_persona_without_humor_examples_empties_them():
    p = A.persona_without_humor_examples({"humor": {"examples": ["a -> 'b'"], "style": "dry"}})
    assert p["humor"]["examples"] == []


def test_persona_without_humor_examples_preserves_everything_else():
    p = A.persona_without_humor_examples(
        {"name": "seth", "humor": {"examples": ["x"], "style": "dry"}, "traits": ["direct"]})
    assert p["name"] == "seth" and p["traits"] == ["direct"]
    assert p["humor"]["style"] == "dry"


def test_persona_without_humor_examples_does_not_mutate_input():
    src = {"humor": {"examples": ["x"]}}
    A.persona_without_humor_examples(src)
    assert src["humor"]["examples"] == ["x"]


def test_persona_without_humor_examples_tolerates_missing_humor():
    assert A.persona_without_humor_examples({"name": "seth"}) == {"name": "seth"}


# ---- sampling --------------------------------------------------------------

_ROWS = [{"incoming": f"m{i}", "context_turns": [{"from": "them", "text": "t"}]}
         for i in range(20)] + [
    {"incoming": "no ctx", "context_turns": []},
    {"incoming": "   ", "context_turns": [{"from": "them", "text": "t"}]},
]


def test_sample_pairs_is_deterministic_for_a_seed():
    assert ([p["incoming"] for p in A.sample_pairs(_ROWS, 5, 1337)] ==
            [p["incoming"] for p in A.sample_pairs(_ROWS, 5, 1337)])


def test_sample_pairs_skips_rows_without_context():
    """Both arms must see the thread; a context-free pair measures the wrong
    thing (see get_ai_response_cli's docstring in eval_blinded_ab.py)."""
    got = [p["incoming"] for p in A.sample_pairs(_ROWS, 50, 1)]
    assert "no ctx" not in got


def test_sample_pairs_skips_blank_incoming():
    assert "   " not in [p["incoming"] for p in A.sample_pairs(_ROWS, 50, 1)]


def test_sample_pairs_respects_n():
    assert len(A.sample_pairs(_ROWS, 4, 1)) == 4


# ---- style measurement -----------------------------------------------------

def test_style_stats_counts_lowercase_starts():
    s = A.style_stats(["yeah ok", "Sure thing", "nah", "Fine"])
    assert s["n"] == 4 and abs(s["lower"] - 0.5) < 1e-9


def test_style_stats_ignores_leading_punctuation_when_finding_first_letter():
    """'...yeah' starts lowercase; '"Sure' does not."""
    s = A.style_stats(["...yeah", '"Sure'])
    assert abs(s["lower"] - 0.5) < 1e-9


def test_style_stats_drops_error_sentinels_and_counts_them():
    s = A.style_stats(["ok", "(error: boom)", "(no choices)", "sure"])
    assert s["n"] == 2 and s["dropped"] == 2


def test_style_stats_returns_none_when_nothing_usable():
    assert A.style_stats(["(error: x)", "(no choices)"]) is None
    assert A.style_stats([]) is None


def test_style_stats_reports_median_length():
    s = A.style_stats(["ab", "abcd", "abcdef"])
    assert s["median_len"] == 4


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
