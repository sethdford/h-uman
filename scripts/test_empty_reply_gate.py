#!/usr/bin/env python3
# scripts/test_empty_reply_gate.py
#
# Pure, model-free tests for scripts/empty_reply_gate.py: the nightly
# empty-reply promotion gate (candidate empty rate must not exceed the
# serving adapter's, measured the same night on the same spare server) and
# the per-candidate promotion_manifest.json that m3_promote.py enforces.
# Run: python3 -m pytest scripts/test_empty_reply_gate.py -q   (or plain python3)
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import empty_reply_gate as g  # noqa: E402


def _report(adapter, empty_all, n_all=72, empty_cls=None, n_cls=36, **over):
    empty_cls = empty_all if empty_cls is None else empty_cls
    r = {"label": "x", "adapter": adapter, "adapter_applied": True,
         "prompts_file": "/repo/scripts/eval_data/empty_reply_prompts.jsonl",
         "samples": 3, "max_tokens": 200, "temperature": 0.7,
         "empty_retry_off_basis": "server process environment: MLX_EMPTY_RETRY off",
         "summary": {"classifier": {"n": n_cls, "empty": empty_cls, "rate": empty_cls / n_cls},
                     "chat": {"n": n_all - n_cls, "empty": empty_all - empty_cls,
                              "rate": (empty_all - empty_cls) / (n_all - n_cls)},
                     "all": {"n": n_all, "empty": empty_all, "rate": empty_all / n_all}}}
    r.update(over)
    return r


def _write(d, name, obj):
    p = Path(d) / name
    p.write_text(json.dumps(obj))
    return str(p)


def test_pass_when_candidate_rate_not_above_serving():
    v = g.decide_empty_reply(_report("/c", 1), _report("/s", 5))
    assert v["verdict"] == "PASS" and v["candidate_rate"] < v["serving_rate"]


def test_equal_rates_pass():
    assert g.decide_empty_reply(_report("/c", 0), _report("/s", 0))["verdict"] == "PASS"


def test_block_when_candidate_rate_above_serving():
    v = g.decide_empty_reply(_report("/c", 6), _report("/s", 5))
    assert v["verdict"] == "BLOCK" and v["reason"] == "candidate_empty_rate_above_serving"


def test_inconclusive_when_arms_not_comparable():
    v = g.decide_empty_reply(_report("/c", 0, samples=5), _report("/s", 5))
    assert v["verdict"] == "INCONCLUSIVE" and "samples" in v["reason"]


def test_inconclusive_when_a_side_is_missing_or_unmeasured():
    assert g.decide_empty_reply(None, _report("/s", 5))["verdict"] == "INCONCLUSIVE"
    bad = _report("/c", 0)
    bad["summary"]["all"]["rate"] = None
    assert g.decide_empty_reply(bad, _report("/s", 5))["verdict"] == "INCONCLUSIVE"


def test_combine_live_requires_both_gates():
    assert g.combine("PASS", {"verdict": "PASS"}, "live") == "PASS"
    assert g.combine("PASS", {"verdict": "BLOCK"}, "live") == "BLOCK"
    assert g.combine("PASS", {"verdict": "INCONCLUSIVE"}, "live") == "INCONCLUSIVE"
    assert g.combine("HOLD", {"verdict": "PASS"}, "live") == "HOLD"


def test_combine_shadow_never_changes_the_authorship_verdict():
    assert g.combine("PASS", {"verdict": "BLOCK"}, "shadow") == "PASS"
    assert g.combine("BLOCK", {"verdict": "PASS"}, "shadow") == "BLOCK"


def test_cli_writes_manifest_with_enforce_in_live(capsys):
    with tempfile.TemporaryDirectory() as d:
        cand = Path(d) / "cand"
        cand.mkdir()
        score = _write(d, "score.json", {"promotion_gate": {"verdict": "PASS"}})
        rc = g.main(["--mode", "live", "--candidate-adapter", str(cand),
                     "--serving-adapter", "/s",
                     "--candidate-json", _write(d, "c.json", _report(str(cand), 6)),
                     "--serving-json", _write(d, "s.json", _report("/s", 5)),
                     "--authorship-json", score, "--out", str(cand / g.MANIFEST_NAME)])
        m = json.loads((cand / g.MANIFEST_NAME).read_text())
        assert rc == 0
        assert m["empty_reply"]["verdict"] == "BLOCK" and m["empty_reply"]["enforce"] is True
        assert m["authorship"]["verdict"] == "PASS"
        assert m["promotion_gate"]["verdict"] == "BLOCK"
        assert "promotion_gate=BLOCK" in capsys.readouterr().out
        assert g.enforced_empty_reply_verdict(str(cand))["verdict"] == "BLOCK"


def test_cli_shadow_records_but_does_not_enforce():
    with tempfile.TemporaryDirectory() as d:
        cand = Path(d) / "cand"
        cand.mkdir()
        g.main(["--mode", "shadow", "--candidate-adapter", str(cand), "--serving-adapter", "/s",
                "--candidate-json", _write(d, "c.json", _report(str(cand), 6)),
                "--serving-json", _write(d, "s.json", _report("/s", 5)),
                "--out", str(cand / g.MANIFEST_NAME)])
        m = json.loads((cand / g.MANIFEST_NAME).read_text())
        assert m["empty_reply"]["verdict"] == "BLOCK" and m["empty_reply"]["enforce"] is False
        assert m["promotion_gate"]["verdict"] == "UNKNOWN"   # no authorship json given
        assert g.enforced_empty_reply_verdict(str(cand)) is None


def test_cli_inconclusive_reason_is_recorded_and_enforced():
    with tempfile.TemporaryDirectory() as d:
        cand = Path(d) / "cand"
        cand.mkdir()
        g.main(["--mode", "live", "--candidate-adapter", str(cand), "--serving-adapter", "/s",
                "--inconclusive", "deadline", "--out", str(cand / g.MANIFEST_NAME)])
        v = g.enforced_empty_reply_verdict(str(cand))
        assert v["verdict"] == "INCONCLUSIVE" and v["reason"] == "deadline"


def test_no_manifest_means_nothing_enforced():
    with tempfile.TemporaryDirectory() as d:
        assert g.enforced_empty_reply_verdict(d) is None
        assert g.enforced_empty_reply_verdict(str(Path(d) / "a-file.bin")) is None


if __name__ == "__main__":
    import inspect
    fails = 0
    for name, fn in sorted(inspect.getmembers(sys.modules[__name__], inspect.isfunction)):
        if not name.startswith("test_"):
            continue
        try:
            if "capsys" in inspect.signature(fn).parameters:
                continue  # pytest-only
            fn()
            print(f"PASS {name}")
        except Exception as e:  # noqa: BLE001
            fails += 1
            print(f"FAIL {name}: {e!r}")
    sys.exit(1 if fails else 0)
