"""Pins for training_loop.split_train_valid — the content-keyed validation split.

Why: the positional split re-drew the validation set whenever the outcome corpus
grew (2026-09-06: 239->257 pairs, val loss 3.438->3.548, regression gate FAIL on
a run that may not have regressed). These pin that the held-out rows are a
function of content, not position, and that the recorded id tracks them.
"""
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import training_loop as tl  # noqa: E402


def _rows(n, tag="r"):
    return [json.dumps({"prompt": f"p{i}-{tag}", "completion": f"c{i}"}) for i in range(n)]


def test_split_is_deterministic_and_disjoint():
    lines = _rows(300)
    t1, v1, id1 = tl.split_train_valid(lines)
    t2, v2, id2 = tl.split_train_valid(lines)
    assert (t1, v1, id1) == (t2, v2, id2)
    assert v1 and id1 and len(id1) == 12
    assert not set(t1) & set(v1)
    assert sorted(t1 + v1) == sorted(lines)


def test_split_is_order_independent():
    lines = _rows(300)
    _, v1, id1 = tl.split_train_valid(lines)
    _, v2, id2 = tl.split_train_valid(list(reversed(lines)))
    assert sorted(v1) == sorted(v2) and id1 == id2


def test_appending_non_validation_rows_keeps_the_validation_set():
    base = _rows(200)
    _, v_before, id_before = tl.split_train_valid(base)
    # rows the split itself classifies as training-side, appended at the end
    extra = [l for l in _rows(120, tag="new") if l not in tl.split_train_valid([l] * 1 + base)[1]]
    extra = [l for l in extra if tl.split_train_valid(base + [l])[1] == v_before][:40]
    assert extra, "fixture needs some appended rows that are not validation candidates"
    _, v_after, id_after = tl.split_train_valid(base + extra)
    assert v_after == v_before and id_after == id_before


def test_positional_split_would_have_moved_but_this_one_does_not():
    base = _rows(200)
    _, v_before, _ = tl.split_train_valid(base)
    shifted = [json.dumps({"prompt": "inserted-first", "completion": "x"})] + base
    _, v_after, _ = tl.split_train_valid(shifted)
    # the inserted row may or may not be a candidate; every ORIGINAL validation row stays
    assert set(v_before) <= set(v_after) | {json.dumps({"prompt": "inserted-first", "completion": "x"})}
    assert len(set(v_before) - set(v_after)) <= 1  # at most displaced by the cap


def test_cap_and_tiny_corpus():
    lines = _rows(5000)
    _, v, _ = tl.split_train_valid(lines, cap=64)
    assert len(v) == 64
    t, v, vid = tl.split_train_valid(_rows(3))
    assert len(v) == 1 and vid and len(t) == 2      # tiny corpus still gets a val row
    t, v, vid = tl.split_train_valid(_rows(1))
    assert v == [] and vid is None and len(t) == 1


def test_read_val_set_id_roundtrip_and_absent():
    with tempfile.TemporaryDirectory() as d:
        p = Path(d)
        assert tl.read_val_set_id(p) is None
        (p / "val_set.json").write_text(json.dumps({"val_set_id": "abc123abc123"}))
        assert tl.read_val_set_id(p) == "abc123abc123"
        (p / "val_set.json").write_text("not json")
        assert tl.read_val_set_id(p) is None


# ---------------------------------------------------------------------------
# 2026-09-12: frozen validation set (the content-keyed id churned 5x in 6 nights)
# ---------------------------------------------------------------------------
def test_frozen_reuses_the_frozen_rows_and_keeps_the_id_across_appends():
    base = _rows(200)
    train0, val0, vid0 = tl.split_train_valid(base)
    frozen = list(val0)
    t1, v1, vid1, refrozen1 = tl.split_train_valid_frozen(base, frozen)
    assert (v1, vid1, refrozen1) == (frozen, vid0, False)
    assert not set(t1) & set(v1) and sorted(t1 + v1) == sorted(base)
    # append 120 rows: the held-out set and its id do not move, new rows all train
    bigger = base + _rows(120, tag="new")
    t2, v2, vid2, refrozen2 = tl.split_train_valid_frozen(bigger, frozen)
    assert v2 == frozen and vid2 == vid0 and refrozen2 is False
    assert len(t2) == len(bigger) - len(frozen)


def test_frozen_refreezes_when_the_corpus_was_rebuilt():
    base = _rows(200)
    _, val0, vid0 = tl.split_train_valid(base)
    rebuilt = _rows(200, tag="rebuilt")            # none of the frozen rows survive
    t, v, vid, refrozen = tl.split_train_valid_frozen(rebuilt, val0)
    assert refrozen is True and v and vid != vid0 and not set(t) & set(v)
    # partial survival above the floor keeps the surviving rows, same contract
    half = base[:100] + _rows(100, tag="x")
    t, v, vid, refrozen = tl.split_train_valid_frozen(half, val0)
    surviving = [l for l in val0 if l in set(half)]
    if len(surviving) >= len(val0) // 2:
        assert refrozen is False and v == surviving
    else:
        assert refrozen is True


def test_frozen_with_no_frozen_rows_falls_back_to_content_keyed_split():
    base = _rows(150)
    t, v, vid, refrozen = tl.split_train_valid_frozen(base, [])
    assert refrozen is True and (t, v, vid) == tl.split_train_valid(base)


def test_frozen_val_path_sits_beside_the_source():
    assert str(tl.frozen_val_path("/x/y/m3-outcomes.jsonl")) == "/x/y/m3-outcomes.jsonl.valid-frozen.jsonl"
