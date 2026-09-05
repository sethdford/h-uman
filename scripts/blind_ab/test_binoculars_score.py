"""Pins the 2026-09-04 classifier-gate crash: `[logsumexp] Received empty array`.

classifier_gate.py writes each trial's context under "context"; load_items read
"incoming", so every item scored context-free. With no context and a tokenizer
that has no BOS (GLM-4.5-Air), the reply started at position 0 and the
predicting slice [resp_start-1, ...) began at -1: an empty array. No test can
load the model, so a fake tokenizer stands in; the arithmetic is what matters.
"""
import json
import os
import sys
import tempfile
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import binoculars_score as bs  # noqa: E402


class NoBosTokenizer:
    bos_token_id = None
    all_special_ids = [999]

    def encode(self, text, add_special_tokens=False):
        return [ord(c) for c in text]

    def apply_chat_template(self, msgs, add_generation_prompt=False):
        # Mirrors a prefix-stable template: user turns are [7]+text+[999],
        # the assistant header [8] appears both as the generation prompt and
        # before assistant content, so prompt_ids is a prefix of full_ids.
        ids = []
        for m in msgs:
            ids += ([7] if m["role"] == "user" else [8]) + self.encode(m["content"]) + [999]
        if add_generation_prompt:
            ids += [8]
        return ids


def test_load_items_accepts_context_key_from_classifier_gate():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "trials.json")
        json.dump({"trials": [{"i": 0, "context": "you up?", "real_seth": "yeah", "ai_response": "yep"}]},
                  open(p, "w"))
        args = types.SimpleNamespace(pairs=p, texts=None)
        items = bs.load_items(args)
    assert len(items) == 2
    assert all(it["context"] == "you up?" for it in items), items


def test_load_items_still_accepts_incoming_key():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "trials.json")
        json.dump([{"i": 1, "incoming": "hey", "real_seth": "hi", "ai_response": "hello"}], open(p, "w"))
        items = bs.load_items(types.SimpleNamespace(pairs=p, texts=None))
    assert all(it["context"] == "hey" for it in items)


def test_no_context_no_bos_never_yields_a_negative_predicting_slice():
    ids, rs, rl = bs.response_token_span(NoBosTokenizer(), "", "yeah")
    assert rs >= 1, (ids, rs, rl)            # position rs-1 must exist
    assert rl >= 1 and rs + rl <= len(ids), (ids, rs, rl)


def test_no_context_no_bos_single_token_reply_is_unscoreable_not_a_crash():
    ids, rs, rl = bs.response_token_span(NoBosTokenizer(), "", "k")
    assert rl == 0, (ids, rs, rl)            # nothing predicts token 0; caller skips


def test_with_context_span_is_the_reply_tokens():
    ids, rs, rl = bs.response_token_span(NoBosTokenizer(), "you up?", "yeah")
    assert ids[rs:rs + rl] == [ord(c) for c in "yeah"]
