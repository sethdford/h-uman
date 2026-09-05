#!/usr/bin/env python3
"""Unit tests for scripts/blind_ab_gate.py (stdlib runner — no pytest dep)."""
import os, sys, json, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blind_ab_gate as g



# Shared by the provenance tests below AND by test_merge_preserves_other_half:
# the runner executes each test as it is defined, so this must come first.
def _serving(adapter, tensors, available=True):
    bound = None
    if adapter and isinstance(tensors, int):
        bound = tensors > 0
    elif adapter is None and available:
        bound = False
    return {"server": "http://127.0.0.1:9", "asked_at": "2026-09-04T00:00:00",
            "model": "fake-base" if available else None,
            "adapter_path": adapter, "tensors_loaded": tensors, "adapter_bound": bound,
            "provenance_available": available,
            "head_sha256": "ab" * 32, "head_bytes": 800}

def test_effective_human_fail_vetoes_proxy_pass():
    proxy = {"verdict": "PASS", "mode": "ENFORCING"}
    human = {"verdict": "FAIL"}
    assert g.compute_effective_verdict(proxy, human) == "FAIL"


def test_effective_both_pass():
    assert g.compute_effective_verdict(
        {"verdict": "PASS", "mode": "ENFORCING"}, {"verdict": "PASS"}) == "PASS"


def test_effective_proxy_enforcing_fail_no_human():
    assert g.compute_effective_verdict(
        {"verdict": "FAIL", "mode": "ENFORCING"}, {"verdict": "ABSENT"}) == "FAIL"


def test_effective_advisory_when_proxy_advisory_no_human():
    assert g.compute_effective_verdict(
        {"verdict": "ADVISORY", "mode": "ADVISORY"}, {"verdict": "ABSENT"}) == "ADVISORY"


def test_effective_human_stale_does_not_pass_on_its_own():
    assert g.compute_effective_verdict(
        {"verdict": "ADVISORY", "mode": "ADVISORY"}, {"verdict": "STALE"}) == "ADVISORY"


def test_proxy_decision_advisory_below_threshold():
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=10.0, n_real_pairs=5, baseline=None,
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert mode == "ADVISORY" and verdict == "ADVISORY" and fail is False


def test_proxy_decision_enforcing_pass():
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=50.0, n_real_pairs=40, baseline=None,
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert mode == "ENFORCING" and verdict == "PASS" and fail is False


def test_proxy_decision_enforcing_fail_under_floor():
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=40.0, n_real_pairs=40, baseline=None,
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert mode == "ENFORCING" and verdict == "FAIL" and fail is True


def test_proxy_decision_enforcing_fail_on_regression():
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=46.0, n_real_pairs=40, baseline={"fool_rate": 55.0},
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert verdict == "FAIL" and fail is True  # 55 - 46 = 9 > 5


def test_merge_preserves_other_half():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "gate.json")
        # An adapter-arm proxy write needs serving provenance now (see the
        # provenance tests below); a bound adapter is the happy path here.
        g.write_proxy_half(p, {"fool_rate": 50.0, "mode": "ENFORCING",
                               "verdict": "PASS", "n_real_pairs": 40,
                               "n_trials": 40, "baseline_fool_rate": None,
                               "fail_under": 45, "max_regression": 5}, commit="abc",
                           serving=_serving("/adapters/seth-v6", 160), claims_adapter=True)
        g.write_human_half(p, {"detection": 0.5, "ci_lo": 0.4, "n": 30,
                               "verdict": "PASS"})
        data = json.load(open(p))
        assert data["proxy"]["fool_rate"] == 50.0
        assert data["human"]["verdict"] == "PASS"
        assert data["effective_verdict"] == "PASS"


def test_eval_gate_synthetic_is_advisory_and_exits_zero():
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as d:
        gate = os.path.join(d, "gate.json")
        env = dict(os.environ, HU_BLIND_AB_GATE_PATH=gate)
        r = subprocess.run(
            ["python3", os.path.join(here, "eval_blinded_ab.py"),
             "--gate", "--gate-dry-run"],
            capture_output=True, text=True, env=env, timeout=60)
        assert r.returncode == 0, r.stderr
        data = json.load(open(gate))
        assert data["proxy"]["mode"] == "ADVISORY"
        assert data["effective_verdict"] == "ADVISORY"


def test_unstamped_human_cannot_grant_pass():
    """A human record with no `tool` stamp is not promotion evidence.

    write_human_half() always stamps `tool`, so an unstamped human block was
    written by something other than the sanctioned writer and nothing can
    vouch for its origin. Observed 2026-07-27: the live gate carried
    {verdict PASS, detection 0.225, n 40} with no `tool`, over a sheet that
    split exactly 20 A / 20 B with zero confidence values — the shape of a
    programmatic fill. Precedent: 2026-07-26, a synthetic run replaced a
    genuine n=12 human verdict with an n=160 machine one.
    """
    stamped = {"tool": "blind_ab/score.py", "verdict": "PASS"}
    unstamped = {"verdict": "PASS"}
    advisory_proxy = {"verdict": "PASS", "mode": "ADVISORY"}

    # stamped human PASS + advisory proxy PASS -> PASS
    assert g.compute_effective_verdict(advisory_proxy, stamped) == "PASS"
    # SAME inputs but unstamped -> must NOT reach PASS on the human's say-so
    assert g.compute_effective_verdict(advisory_proxy, unstamped) == "ADVISORY"

    # ...but an unstamped FAIL MUST still veto. The asymmetry is deliberate:
    # both directions fail closed toward NOT promoting. Downgrading a FAIL
    # would let anyone erase a veto by writing an unsanctioned record.
    assert g.compute_effective_verdict(
        {"verdict": "PASS", "mode": "ENFORCING"}, {"verdict": "FAIL"}) == "FAIL"
    assert g.compute_effective_verdict(
        {"verdict": "PASS", "mode": "ENFORCING"},
        {"tool": "blind_ab/score.py", "verdict": "FAIL"}) == "FAIL"

    assert g.human_is_attributable(stamped)
    assert not g.human_is_attributable(unstamped)
    assert not g.human_is_attributable({})
    assert not g.human_is_attributable(None)


def test_score_emit_gate_writes_human_half():
    import subprocess, csv
    here = os.path.dirname(os.path.abspath(__file__))
    score = os.path.join(here, "blind_ab", "score.py")
    with tempfile.TemporaryDirectory() as d:
        gate = os.path.join(d, "gate.json")
        sheet = os.path.join(d, "sheet.csv")
        keyf = os.path.join(d, "key.json")
        with open(sheet, "w", newline="") as f:
            w = csv.writer(f); w.writerow(["id", "choice", "confidence"])
            # Create data: 2 correct, 2 incorrect -> 0.5 detection (PASS)
            for i in range(4):
                choice = "A" if i < 2 else "B"
                w.writerow([str(i), choice, "4"])
        json.dump({str(i): "A" for i in range(4)}, open(keyf, "w"))
        # HOME=tmpdir: score.py also writes ~/.human/blind_ab_gate.json for
        # the C promotion gate; the test must not touch the real one.
        env = dict(os.environ, HOME=d)
        r = subprocess.run(
            ["python3", score, sheet, "--key", keyf,
             "--rater", "human", "--emit-gate", gate],
            capture_output=True, text=True, env=env, timeout=60)
        assert r.returncode == 0, f"returncode={r.returncode}, stderr={r.stderr}"
        data = json.load(open(gate))
        assert data["human"]["verdict"] in ("PASS", "FAIL")
        assert data["human"]["n"] == 4


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for fn in fns:
        try:
            fn(); print(f"PASS {fn.__name__}")
        except Exception as e:
            failed += 1; print(f"FAIL {fn.__name__}: {e}")
    print(f"\n{len(fns)-failed}/{len(fns)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    _run()


# ---- serving provenance at verdict time ------------------------------------
# 2026-07-26 -> 09-04 the adapter on :8741 bound 0 tensors while /health said
# applied; the gate's provenance was annotated post-hoc. The writer now REFUSES
# to emit a verdict it cannot attribute to a bound adapter.


def test_provenance_refusal_names_adapter_but_binds_nothing():
    reason = g.proxy_provenance_refusal(_serving("/adapters/seth-v6", 0), claims_adapter=True)
    assert reason and "nothing bound" in reason


def test_provenance_refusal_unreachable_when_adapter_claimed():
    reason = g.proxy_provenance_refusal(_serving(None, None, available=False), claims_adapter=True)
    assert reason and "unreachable" in reason


def test_provenance_refusal_unreachable_is_allowed_for_base_arm():
    assert g.proxy_provenance_refusal(_serving(None, None, available=False), claims_adapter=False) is None


def test_provenance_refusal_no_adapter_when_adapter_claimed():
    reason = g.proxy_provenance_refusal(_serving(None, 0), claims_adapter=True)
    assert reason and "no adapter" in reason


def test_provenance_refusal_none_when_adapter_bound():
    assert g.proxy_provenance_refusal(_serving("/adapters/seth-v6", 160), claims_adapter=True) is None


def test_provenance_refusal_missing_serving_when_adapter_claimed():
    reason = g.proxy_provenance_refusal(None, claims_adapter=True)
    assert reason and "no serving provenance" in reason


def _existing_gate(d):
    path = os.path.join(d, "gate.json")
    with open(path, "w") as f:
        json.dump({"schema_version": 1, "commit": "deadbeef",
                   "proxy": {"tool": "eval_blinded_ab.py", "verdict": "PASS", "mode": "ENFORCING",
                             "fool_rate": 51.0, "n_trials": 43, "n_real_pairs": 43},
                   "human": {"verdict": "ABSENT"}, "effective_verdict": "PASS"}, f, indent=2)
    return path, open(path, "rb").read()


def test_write_proxy_half_refuses_bound_nothing_and_leaves_gate_byte_identical():
    d = tempfile.mkdtemp()
    path, before = _existing_gate(d)
    try:
        g.write_proxy_half(path, {"verdict": "PASS", "mode": "ENFORCING", "fool_rate": 60.0,
                                  "n_trials": 40, "n_real_pairs": 40},
                           serving=_serving("/adapters/seth-v6", 0), claims_adapter=True)
    except g.ProvenanceRefusal as e:
        assert "nothing bound" in str(e)
    else:
        raise AssertionError("write_proxy_half wrote a verdict for an adapter that bound 0 tensors")
    assert open(path, "rb").read() == before


def test_write_proxy_half_refuses_without_serving_when_adapter_claimed():
    d = tempfile.mkdtemp()
    path, before = _existing_gate(d)
    try:
        g.write_proxy_half(path, {"verdict": "PASS", "mode": "ENFORCING", "fool_rate": 60.0,
                                  "n_trials": 40, "n_real_pairs": 40})
    except g.ProvenanceRefusal:
        pass
    else:
        raise AssertionError("a verdict with no provenance at all was written")
    assert open(path, "rb").read() == before


def test_write_proxy_half_records_serving_provenance_when_bound():
    d = tempfile.mkdtemp()
    path, _ = _existing_gate(d)
    g.write_proxy_half(path, {"verdict": "PASS", "mode": "ENFORCING", "fool_rate": 60.0,
                              "n_trials": 40, "n_real_pairs": 40,
                              "run_mode": "mlx", "judge_model": "gemini-3.1-pro-preview"},
                       serving=_serving("/adapters/seth-v6", 160), claims_adapter=True)
    proxy = json.load(open(path))["proxy"]
    s = proxy["serving"]
    assert s["adapter_path"] == "/adapters/seth-v6"
    assert s["tensors_loaded"] == 160 and s["adapter_bound"] is True
    assert s["model"] == "fake-base" and len(s["head_sha256"]) == 64
    assert proxy["claims_adapter"] is True
    assert proxy["run_mode"] == "mlx" and proxy["judge_model"] == "gemini-3.1-pro-preview"
    assert proxy["n_trials"] == 40 and proxy["n_real_pairs"] == 40


def test_write_proxy_half_base_arm_records_no_adapter_without_refusing():
    d = tempfile.mkdtemp()
    path, _ = _existing_gate(d)
    g.write_proxy_half(path, {"verdict": "ADVISORY", "mode": "ADVISORY", "fool_rate": None,
                              "n_trials": 0, "n_real_pairs": 0},
                       serving=_serving(None, 0), claims_adapter=False)
    proxy = json.load(open(path))["proxy"]
    assert proxy["claims_adapter"] is False and proxy["serving"]["adapter_bound"] is False
