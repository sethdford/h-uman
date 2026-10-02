"""Hermetic tests for scripts/cutover/cutover_plan.py (gate detection, arms,
runtime estimate, probe preparation) and scripts/cutover/judge_local.py
(loopback-only, reasoning_effort=none, pacing). No network, no real data."""
import json
import os
import stat
import sys
import types

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.join(HERE, "..", "scripts", "cutover")
sys.path.insert(0, KIT)
import cutover_plan as cp  # noqa: E402
import judge_local as jl  # noqa: E402


def test_detect_gates_reads_names_out_of_the_binary_in_canonical_order(tmp_path):
    b = tmp_path / "human"
    b.write_bytes(b"\x00\x01HU_DIRECTOR_V2\x00junk HU_THREAD_CONTEXT\x00HU_GRIEF_DECAY\x00"
                  b"HU_THREAD_CONTEXT_X\x00HU_LENGTH_POLICYZ\x00")
    # HU_GRIEF_DECAY is excluded; suffixed lookalikes are different tokens
    assert cp.detect_gates(str(b)) == ["HU_THREAD_CONTEXT", "HU_DIRECTOR_V2"]


def test_build_arms_shapes():
    arms = dict(cp.build_arms(["HU_THREAD_CONTEXT", "HU_DIRECTOR_V2"]))
    assert list(arms) == ["A", "B", "B-no-thread_context", "B-no-director_v2"]
    assert arms["A"] == {"HU_THREAD_CONTEXT": "off", "HU_DIRECTOR_V2": "off"}
    assert arms["B"] == {"HU_THREAD_CONTEXT": "live", "HU_DIRECTOR_V2": "live"}
    assert arms["B-no-director_v2"] == {"HU_THREAD_CONTEXT": "live", "HU_DIRECTOR_V2": "off"}
    assert cp.gate_for_arm("B-no-director_v2") == "HU_DIRECTOR_V2"
    assert cp.gate_for_arm("A") is None


def test_build_arms_refuses_non_candidates_and_nothing():
    with pytest.raises(ValueError):
        cp.build_arms(["HU_POST_SEND_DEFER"])
    with pytest.raises(ValueError):
        cp.build_arms([])


def test_arm_spec_is_what_replay_driver_parses():
    sys.path.insert(0, os.path.join(HERE, "..", "scripts", "blind_ab"))
    spec = cp.arm_spec("B-no-x", {"HU_A": "live", "HU_B": "off"})
    assert spec == "B-no-x:HU_A=live,HU_B=off"
    try:
        import replay_driver  # present once the replay harness (#594) is merged
    except ImportError:
        return
    assert replay_driver.parse_arm(spec) == ("B-no-x", {"HU_A": "live", "HU_B": "off"})


def test_estimate_counts_every_phase():
    e = cp.estimate(turns=60, probes=50, n_arms=9, mem_arms=2, delay_ms=3000, judge_pace_s=2)
    assert e["turn_arms"] == 540 and e["probe_arms"] == 100
    assert e["generation_s"] == 540 * cp.SEC_PER_TURN_ARM
    assert e["memory_s"] == 100 * cp.SEC_PER_PROBE_ARM
    assert e["judge_items"] == round(60 * cp.TEXT_SHARE) * 9
    assert e["total_s"] == e["generation_s"] + e["memory_s"] + e["judge_s"]
    slower = cp.estimate(60, 50, 9, 2, delay_ms=5000, judge_pace_s=2)
    assert slower["generation_s"] == 540 * (cp.SEC_PER_TURN_ARM + 2)
    assert cp.fmt_dur(3 * 3600 + 25 * 60) == "3h25m" and cp.fmt_dur(600) == "10m"


def probe(i, cat):
    return {"id": f"p{i}", "contact_id": "x@bench.invalid", "ts": "2023-05-21 14:12:00",
            "inbound_bubbles": ["q?"], "history": [],
            "probe": {"category": cat, "gold_answers": ["a"]}}


def test_prep_probes_balances_categories_and_writes_epoch_ts_privately(tmp_path):
    src = tmp_path / "probes.jsonl"
    cats = ["temporal"] * 6 + ["single_hop"] * 6 + ["adversarial"] * 1
    src.write_text("".join(json.dumps(probe(i, c)) + "\n" for i, c in enumerate(cats))
                   + json.dumps({"id": "not-a-probe"}) + "\n")
    dst = tmp_path / "turns.jsonl"
    n, by_cat = cp.prep_probes(str(src), str(dst), 7)
    assert n == 7 and by_cat == {"adversarial": 1, "single_hop": 3, "temporal": 3}
    rows = [json.loads(line) for line in dst.read_text().splitlines()]
    assert all(isinstance(r["ts"], int) and r["probe"] for r in rows)
    assert stat.S_IMODE(os.stat(dst).st_mode) == 0o600
    with pytest.raises(ValueError):
        cp.prep_probes(str(tmp_path / "turns.jsonl"), str(tmp_path / "x"), 0)


def test_cli_arms_only_filter(capsys):
    assert cp.main(["arms", "--gates", "HU_THREAD_CONTEXT,HU_DIRECTOR_V2", "--only", "A,B"]) == 0
    out = capsys.readouterr().out.split()
    assert out == ["A:HU_THREAD_CONTEXT=off,HU_DIRECTOR_V2=off",
                   "B:HU_THREAD_CONTEXT=live,HU_DIRECTOR_V2=live"]


# ── judge_local ────────────────────────────────────────────────────────

def test_judge_refuses_cloud_endpoints_and_api_override(capsys):
    assert jl.main(["s.csv", "--out", "o.csv", "--harness-dir", ".",
                    "--endpoint", "https://api.example.com/v1/chat/completions"]) == 2
    assert jl.main(["s.csv", "--out", "o.csv", "--harness-dir", ".",
                    "--endpoint", "http://127.0.0.1:11434/v1", "--api", "anthropic"]) == 2
    assert jl.is_loopback("http://[::1]:11434/v1") and jl.is_loopback("http://localhost:1/")
    assert not jl.is_loopback("http://127.0.0.1.example.com/v1")


def test_judge_install_injects_reasoning_effort_paces_and_refuses_cloud_urls(monkeypatch):
    seen = {}

    class Req:
        def __init__(self, url, data=None, headers=None, **kw):
            seen["url"], seen["body"] = url, json.loads(data)

    sj = types.SimpleNamespace()
    sj.urllib = types.SimpleNamespace(request=types.SimpleNamespace(Request=Req))

    def call(endpoint):
        sj.urllib.request.Request(endpoint, data=json.dumps({"model": "m"}).encode())
        return "ok"

    sj.call = call
    sleeps = []
    monkeypatch.setattr(jl.time, "sleep", sleeps.append)
    jl.install(sj, pace_s=2.0)
    assert sj.call("http://127.0.0.1:11434/v1/chat/completions") == "ok"
    assert seen["body"] == {"model": "m", "reasoning_effort": "none"}
    assert sleeps == [2.0]
    with pytest.raises(RuntimeError):
        sj.call("https://cloud.example.com/v1/chat/completions")
    assert sleeps == [2.0, 2.0]  # paced even on failure
