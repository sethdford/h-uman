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
