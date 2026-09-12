"""Pins for dpo_results.regression_verdict's val_set_id scoping."""
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import dpo_results as dr  # noqa: E402


def _h(val_loss, vid=None):
    return {"val_loss": val_loss, "val_set_id": vid}


def test_history_on_a_different_validation_set_is_not_compared():
    history = [_h(3.384, "oldsplit0001"), _h(3.438)]  # July record + a pre-id record
    assert dr.regression_verdict(history, {"val_loss": 3.548, "val_set_id": "newsplit0002"}) == "FIRST_RUN"


def test_same_validation_set_is_compared_and_can_fail():
    history = [_h(3.384, "sameid000001")]
    assert dr.regression_verdict(history, {"val_loss": 3.548, "val_set_id": "sameid000001"}) == "FAIL"
    assert dr.regression_verdict(history, {"val_loss": 3.40, "val_set_id": "sameid000001"}) == "PASS"


def test_legacy_result_without_id_keeps_the_old_comparison():
    history = [_h(3.384, "x"), _h(3.438)]
    assert dr.regression_verdict(history, {"val_loss": 3.548}) == "FAIL"
    assert dr.regression_verdict(history, {"val_loss": 3.40}) == "PASS"


def test_append_result_records_val_set_id():
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "r.jsonl"
        dr.append_result(f, "2026-09-06T00:00:00", "a", {"outcomes": 1}, 1.0, 2.0, None, 2.0, 10, "deadbeef",
                         val_set_id="abc")
        dr.append_result(f, "2026-09-06T00:00:01", "b", {"outcomes": 1}, 1.0, 2.0, None, 2.0, 10, "deadbeef")
        recs = [json.loads(l) for l in f.read_text().splitlines()]
        assert recs[0]["val_set_id"] == "abc" and recs[1]["val_set_id"] is None


def test_history_excluding_drops_the_run_being_judged():
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "r.jsonl"
        for aid, vl in (("a", 3.3), ("b", 3.5), ("c", 3.9)):
            dr.append_result(f, "2026-09-12T03:00:00", aid, {"outcomes": 1}, 1.0, vl, None, 2.0, 10, "x",
                             val_set_id="same")
        h = dr.history_excluding(f, "c")
        assert sorted(r["adapter_id"] for r in h) == ["a", "b"]
        # with itself excluded, c is judged against a and b -> FAIL (3.9 > 3.3 + 0.1)
        assert dr.regression_verdict(h, {"val_loss": 3.9, "val_set_id": "same"}) == "FAIL"
        # the 2026-09-07..12 shape: a fresh val_set_id whose ONLY record is the run
        # itself -> min is itself -> PASS instead of FIRST_RUN
        dr.append_result(f, "2026-09-12T03:00:01", "d", {"outcomes": 1}, 1.0, 3.9, None, 2.0, 10, "x",
                         val_set_id="fresh")
        assert dr.regression_verdict(dr.load_recent(f), {"val_loss": 3.9, "val_set_id": "fresh"}) == "PASS"
        assert dr.regression_verdict(dr.history_excluding(f, "d"), {"val_loss": 3.9, "val_set_id": "fresh"}) == "FIRST_RUN"
