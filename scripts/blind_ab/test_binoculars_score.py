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


# ---- DivEye variability features (arXiv 2509.18880) ------------------------

def test_surprisal_diversity_is_zero_for_flat_and_single_token_series():
    assert bs.surprisal_diversity([0.7, 0.7, 0.7, 0.7]) == (0.0, 0.0, 0.0)
    assert bs.surprisal_diversity([4.2]) == (0.0, 0.0, 0.0)
    assert bs.surprisal_diversity([]) == (0.0, 0.0, 0.0)


def test_surprisal_diversity_orders_bursty_above_smooth():
    # Same mean surprisal (2.0), very different variability: the human-shaped
    # series must score higher on every DivEye feature than the flat one.
    smooth = [1.9, 2.0, 2.1, 2.0, 1.9, 2.1]
    bursty = [0.2, 3.8, 0.2, 3.8, 0.2, 3.8]
    s_std, s_burst, _ = bs.surprisal_diversity(smooth)
    b_std, b_burst, _ = bs.surprisal_diversity(bursty)
    assert b_std > s_std and b_burst > s_burst


def test_combine_emits_diveye_keys_from_the_real_token_series():
    import numpy as np
    with tempfile.TemporaryDirectory() as d:
        # 3 reply tokens over a 4-token vocab; token i is predicted at row i.
        base = np.log(np.array([[0.7, 0.1, 0.1, 0.1],
                                [0.1, 0.1, 0.7, 0.1],
                                [0.1, 0.7, 0.1, 0.1]], dtype=np.float32))
        adpt = np.log(np.full((3, 4), 0.25, dtype=np.float32))  # flat: no variability
        np.save(os.path.join(d, "base_00000.npy"), base)
        np.save(os.path.join(d, "adapted_00000.npy"), adpt)
        items = [{"text": "abc", "context": "", "label": "real"}]
        meta = [{"idx": 0, "resp_tokens": [0, 2, 1], "n_tokens": 3}]
        (r,) = bs.combine(items, meta, d)
    # base predicts every chosen token at p=0.7 -> flat surprisal -> zero variability
    assert r["div_std_base"] == 0.0 and r["div_burst_base"] == 0.0
    assert r["div_std_adapted"] == 0.0
    assert r["score_diveye"] == r["div_std_base"]
    for k in ("div_kurt_base", "div_kurt_adapted", "div_burst_adapted"):
        assert k in r
    # the analyzer knows the new feature names
    assert "diveye std (base)" in bs.SCORE_DEFS


def test_combine_diveye_separates_a_peaked_reply_from_a_flat_one():
    import numpy as np
    with tempfile.TemporaryDirectory() as d:
        # item 0: chosen tokens at p = .9, .05, .9  (bursty surprisal)
        # item 1: chosen tokens at p = .5, .5, .5   (flat surprisal)
        b0 = np.log(np.array([[0.9, 0.05, 0.03, 0.02],
                              [0.9, 0.05, 0.03, 0.02],
                              [0.9, 0.05, 0.03, 0.02]], dtype=np.float32))
        b1 = np.log(np.array([[0.5, 0.3, 0.1, 0.1]] * 3, dtype=np.float32))
        flat = np.log(np.full((3, 4), 0.25, dtype=np.float32))
        np.save(os.path.join(d, "base_00000.npy"), b0)
        np.save(os.path.join(d, "base_00001.npy"), b1)
        np.save(os.path.join(d, "adapted_00000.npy"), flat)
        np.save(os.path.join(d, "adapted_00001.npy"), flat)
        items = [{"text": "x", "context": "", "label": "real"},
                 {"text": "y", "context": "", "label": "ai"}]
        meta = [{"idx": 0, "resp_tokens": [0, 1, 0], "n_tokens": 3},
                {"idx": 1, "resp_tokens": [0, 0, 0], "n_tokens": 3}]
        r0, r1 = bs.combine(items, meta, d)
    assert r0["div_std_base"] > r1["div_std_base"] == 0.0
    assert r0["div_burst_base"] > r1["div_burst_base"] == 0.0
