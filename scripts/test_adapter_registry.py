#!/usr/bin/env python3
"""Registry eval rows: a score without evidence is ABSENT, not a number.

2026-07-12..07-25, seth-lora-v4-repair accumulated 14 consecutive
{"score": 1.0, "verdict": "SKIP"} rows with no n and no reason. A reader
that trusts `score` sees 14 perfect evals; the truth was a saturated shape
classifier scoring the literal "[timeout]" string. Every reader that feeds a
promotion or a gate must go through eval_is_measured()/latest_measured_eval(),
and the writer must refuse to record a score without n >= 1 and a reason.
"""
import json
import os
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adapter_registry as R

V4_REPAIR_ROW = {"timestamp": "2026-07-12T09:15:26", "eval_name": "fidelity-nightly",
                 "score": 1.0, "verdict": "SKIP"}
MEASURED_ROW = {"timestamp": "2026-07-11T09:15:26", "eval_name": "fidelity-nightly",
                "score": 0.82, "verdict": "PASS", "n": 40,
                "reason": "delta_mean 0.12 >= floor 0.05; stat pass"}


def _registry_file(rows, adapter_id="seth-lora-v4-repair"):
    d = Path(tempfile.mkdtemp())
    p = d / "registry.json"
    p.write_text(json.dumps({"schema_version": 1, "timestamp": "t",
                             "adapters": {adapter_id: {"created": "t", "training": [],
                                                       "eval": rows}}}))
    return p


def test_eval_is_measured_requires_n_and_reason():
    assert R.eval_is_measured(V4_REPAIR_ROW) is False
    assert R.eval_is_measured({**MEASURED_ROW, "n": 0}) is False
    assert R.eval_is_measured({k: v for k, v in MEASURED_ROW.items() if k != "reason"}) is False
    assert R.eval_is_measured({k: v for k, v in MEASURED_ROW.items() if k != "n"}) is False
    assert R.eval_is_measured(MEASURED_ROW) is True


def test_reader_latest_measured_eval_treats_unevidenced_score_as_absent():
    entry = {"eval": [V4_REPAIR_ROW] * 14}
    assert R.latest_measured_eval(entry) is None
    entry = {"eval": [MEASURED_ROW] + [V4_REPAIR_ROW] * 14}
    latest = R.latest_measured_eval(entry)
    assert latest is not None and latest["score"] == 0.82 and latest["n"] == 40


def test_reader_status_reports_unevidenced_score_as_absent_not_1_0():
    p = _registry_file([V4_REPAIR_ROW] * 14)
    out = R.status(registry_path=p, live_adapter_id="seth-lora-v4-repair")
    assert "Score: 1.0" not in out
    assert "ABSENT" in out
    p2 = _registry_file([MEASURED_ROW])
    out2 = R.status(registry_path=p2, live_adapter_id="seth-lora-v4-repair")
    assert "Score: 0.82" in out2 and "n=40" in out2


def test_writer_refuses_score_without_n_and_reason():
    p = _registry_file([])
    before = p.read_bytes()
    with pytest.raises(ValueError):
        R.record_eval(registry_path=p, adapter_id="seth-lora-v4-repair",
                      eval_name="fidelity-nightly", score=1.0, verdict="SKIP")
    with pytest.raises(ValueError):
        R.record_eval(registry_path=p, adapter_id="seth-lora-v4-repair",
                      eval_name="fidelity-nightly", score=0.9, verdict="PASS", n=0,
                      reason="x", adapter_path="/a", base="b", provenance={})
    assert p.read_bytes() == before


def test_writer_records_n_reason_adapter_path_base_provenance():
    p = _registry_file([])
    R.record_eval(registry_path=p, adapter_id="seth-lora-v4-repair",
                  eval_name="fidelity-nightly", score=0.82, verdict="PASS", n=40,
                  reason="delta_mean 0.12 >= floor", adapter_path="/adapters/v4",
                  base="mlx-community/GLM-4.5-Air-4bit",
                  provenance={"generation": {"mode": "served"}})
    row = json.loads(p.read_text())["adapters"]["seth-lora-v4-repair"]["eval"][-1]
    assert row["n"] == 40 and row["reason"].startswith("delta_mean")
    assert row["adapter_path"] == "/adapters/v4" and row["base"].endswith("4bit")
    assert row["provenance"] == {"generation": {"mode": "served"}}
    assert R.eval_is_measured(row) is True


def test_writer_skip_with_null_score_still_needs_n_and_reason():
    p = _registry_file([])
    with pytest.raises(ValueError):
        R.record_eval(registry_path=p, adapter_id="a", eval_name="fidelity-nightly",
                      score=None, verdict="SKIP")
    R.record_eval(registry_path=p, adapter_id="a", eval_name="fidelity-nightly",
                  score=None, verdict="SKIP", n=25, reason="delta below floor",
                  adapter_path="/a", base="b", provenance={})
    row = json.loads(p.read_text())["adapters"]["a"]["eval"][-1]
    assert row["score"] is None and row["n"] == 25 and row["reason"]
