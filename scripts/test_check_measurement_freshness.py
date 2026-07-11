"""Tests for check_measurement_freshness.py — the CI evidence-recency gate."""

import json

import check_measurement_freshness as cmf


def write_gate(tmp_path, proxy=None, human=None):
    gate = {"schema_version": 1, "proxy": proxy or {}, "human": human or {}}
    p = tmp_path / "gate.json"
    p.write_text(json.dumps(gate))
    return str(p)


NOW = "2026-07-11T12:00:00"


def run(path, now=NOW, max_age=14):
    return cmf.main(["--gate", path, "--now", now, "--max-age-days", str(max_age)])


def test_fresh_enforcing_proxy_passes(tmp_path):
    p = write_gate(tmp_path, proxy={"mode": "ENFORCING", "timestamp": "2026-07-10T00:00:00"})
    assert run(p) == 0


def test_stale_enforcing_proxy_fails(tmp_path):
    p = write_gate(tmp_path, proxy={"mode": "ENFORCING", "timestamp": "2026-05-31T23:06:00"})
    assert run(p) == 1


def test_advisory_proxy_cannot_satisfy_even_when_fresh(tmp_path):
    p = write_gate(tmp_path, proxy={"mode": "ADVISORY", "timestamp": NOW})
    assert run(p) == 1


def test_fresh_human_tier_passes_despite_stale_proxy(tmp_path):
    p = write_gate(
        tmp_path,
        proxy={"mode": "ENFORCING", "timestamp": "2026-01-01T00:00:00"},
        human={"n": 12, "timestamp": "2026-07-09T00:00:00"},
    )
    assert run(p) == 0


def test_human_tier_with_zero_ratings_does_not_count(tmp_path):
    p = write_gate(tmp_path, human={"n": 0, "timestamp": NOW})
    assert run(p) == 1


def test_missing_file_fails(tmp_path):
    assert run(str(tmp_path / "nope.json")) == 1


def test_malformed_timestamp_treated_as_absent(tmp_path):
    p = write_gate(tmp_path, proxy={"mode": "ENFORCING", "timestamp": "not-a-date"})
    assert run(p) == 1


def test_boundary_exactly_at_limit_passes(tmp_path):
    p = write_gate(tmp_path, proxy={"mode": "ENFORCING", "timestamp": "2026-06-27T12:00:00"})
    assert run(p, max_age=14) == 0
