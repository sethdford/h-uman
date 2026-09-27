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
    # 50 % at the default n: powered (half-width 4.9 pp <= 5) and nothing
    # indicts it.
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=50.0, n_real_pairs=g.DEFAULT_MAX_TRIALS, baseline=None,
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert mode == "ENFORCING" and verdict == "PASS" and fail is False


def test_a_perfect_model_can_actually_reach_pass():
    """PASS must be REACHABLE at the sample size the nightly actually runs.

    The first draft of the interval rule required ci_lo >= fail_under -- i.e.
    PROVE the rate exceeds the floor. That makes PASS unreachable: a perfectly
    indistinguishable model (true 50 %) has ci_lo 44.4 at n=300, so the gate
    would have sat at ADVISORY forever and no capability gated on a blind_ab
    PASS could ever unlock. The floor is a red line not to fall below, not a
    target to clear.

    This pins the reachability property itself, so a future tightening of the
    rule cannot quietly make PASS impossible again.

    n=300 is the discriminating case and is used on purpose: there a perfect
    model's lower bound is 44.4, BELOW the floor, so the discarded
    ci_lo >= fail_under rule would return ADVISORY while the evidence-of-harm
    rule correctly returns PASS. (At the n=400 default the bound is 45.1 and
    both rules agree, which is exactly why that n proves nothing here.)
    """
    lo, hi = g.fool_rate_ci(50.0, 300)
    assert lo < 45.0, "precondition: a perfect model's lower bound dips below the floor"
    assert (hi - lo) / 2 <= 6.0, "precondition: n=300 is not absurdly coarse"
    _m, verdict, fail = g.proxy_gate_decision(
        fool_rate=50.0, n_real_pairs=300, baseline=None, n_trials=300,
        max_ci_half_width=6.0)
    assert verdict == "PASS" and fail is False


def test_underpowered_run_cannot_certify_a_pass():
    """No evidence of harm is not evidence of no harm at a coarse resolution.

    50 % at n=50 indicts nothing, but its half-width is 13.3 pp -- it cannot
    separate the 45 % floor from the 50 % target, so PASS would be unearned.
    """
    lo, hi = g.fool_rate_ci(50.0, 50)
    assert (hi - lo) / 2 > g.MAX_CI_HALF_WIDTH_PP
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=50.0, n_real_pairs=50, baseline=None, n_trials=50)
    assert mode == "ENFORCING" and verdict == "ADVISORY" and fail is False


def test_evidence_of_harm_beats_low_power():
    """A small sample cannot prove a model is good, but CAN prove one is bad.

    20 % at n=50: half-width 11 pp (underpowered for PASS) but the whole
    interval tops out at 32.6 %, far under the floor. The power gate must not
    swallow a real indictment, so FAIL is checked first.
    """
    _lo, hi = g.fool_rate_ci(20.0, 50)
    assert hi < 45.0
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=20.0, n_real_pairs=50, baseline=None, n_trials=50)
    assert mode == "ENFORCING" and verdict == "FAIL" and fail is True


def test_proxy_decision_underpowered_sample_is_advisory_not_fail():
    """40 % at n=40 is NOT evidence of a failure -- its CI is [26.3, 55.5].

    Until 2026-09-20 this exact case asserted FAIL, and that assertion was the
    bug: the old rule compared the point estimate to the floor with no regard
    for n, so it failed a model whose interval comfortably contains the floor.
    Simulated over 4000 draws, that rule failed a PERFECTLY indistinguishable
    (true 50 %) model 24.8 % of the time at n=50. Live consequence: the nightly
    read 42.0 % FAIL on 09-19 and 46.0 % PASS on 09-20 off the same adapter and
    judge, and W16 sat red for 5 days.

    ADVISORY is the honest verdict -- not enough evidence to decide -- and it
    does not promote (compute_effective_verdict only promotes on PASS).
    """
    lo, hi = g.fool_rate_ci(40.0, 40)
    assert lo < 45.0 < hi, "precondition: this sample cannot resolve the floor"
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=40.0, n_real_pairs=40, baseline=None,
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    # mode stays ENFORCING: the run is REAL, it just did not resolve the floor.
    assert mode == "ENFORCING" and verdict == "ADVISORY" and fail is False


def test_proxy_decision_enforcing_fail_when_ci_clears_the_floor():
    """The gate still has teeth: a real regression at adequate n FAILs.

    30 % at n=300 has CI [25.1, 35.4] -- entirely below the 45 % floor. This is
    the case the CI rule must NOT go soft on, and the reason DEFAULT_MAX_TRIALS
    rose to 300 alongside it: at n=50 the same true rate is caught only 56.9 %
    of the time, at n=300 it is caught 100 %.
    """
    lo, hi = g.fool_rate_ci(30.0, 300)
    assert hi < 45.0, "precondition: the whole interval is below the floor"
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=30.0, n_real_pairs=300, baseline=None,
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert mode == "ENFORCING" and verdict == "FAIL" and fail is True


def test_proxy_decision_enforcing_fail_on_regression():
    # 35 % against a 55 % baseline at n=300: CI upper 40.6 % is below the
    # allowance line (55 - 5 = 50), so the drop is confidently past allowance.
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=35.0, n_real_pairs=300, baseline={"fool_rate": 55.0},
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert verdict == "FAIL" and fail is True


def test_proxy_decision_regression_within_noise_is_not_a_regression():
    """A 9-point drop at n=40 used to FAIL; its CI reaches 55.5 %.

    Same defect as the floor comparison, on the baseline axis: subtracting two
    point estimates and comparing to an allowance ignores that both are noisy.
    """
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=46.0, n_real_pairs=40, baseline={"fool_rate": 55.0},
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert mode == "ENFORCING" and verdict == "ADVISORY" and fail is False


def test_inconclusive_verdict_still_counts_as_a_real_measurement():
    """An ADVISORY *verdict* must not read as an absent *measurement*.

    check_measurement_freshness.py counts the proxy tier only when
    mode != ADVISORY, because dry runs write ADVISORY mode. When the interval
    rule first landed it returned ADVISORY for BOTH axes on a straddle, which
    would have made a real 300-trial run invisible to the freshness gate and
    kept W16 red permanently -- the exact failure the rule exists to end.

    The two axes are independent: mode = "was this real", verdict = "what did
    it decide". This pins that separation.
    """
    import check_measurement_freshness as fresh
    mode, verdict, fail = g.proxy_gate_decision(
        fool_rate=46.0, n_real_pairs=300, baseline=None,
        fail_under=45.0, max_regression=5.0, enforce_min_pairs=30)
    assert (mode, verdict, fail) == ("ENFORCING", "ADVISORY", False)
    ts, tier = fresh.freshest_real_measurement({
        "proxy": {"mode": mode, "verdict": verdict,
                  "timestamp": "2026-09-20T04:50:44"},
        "human": {"n": 0},
    })
    assert tier == "proxy" and ts is not None, "inconclusive run must still be fresh"


def test_wilson_and_ci_helpers():
    # Point estimate is returned first and is exact; bounds bracket it.
    p, lo, hi = g.wilson(23, 50)
    assert abs(p - 0.46) < 1e-9 and lo < p < hi
    # n == 0 constrains nothing -- must not read as a confident 0 %.
    assert g.wilson(0, 0) == (0.0, 0.0, 0.0)
    assert g.fool_rate_ci(0.0, 0) == (0.0, 100.0)
    # More evidence -> strictly tighter interval at the same rate.
    lo50, hi50 = g.fool_rate_ci(46.0, 50)
    lo300, hi300 = g.fool_rate_ci(46.0, 300)
    assert (hi300 - lo300) < (hi50 - lo50)


def test_default_max_trials_can_resolve_the_decision_boundary():
    """The verdict rule and the sample size are one change, not two.

    proxy_gate_decision is only safe at an n whose interval is near the 5 pp
    boundary it decides on; at n=50 the half-width is 13.3 pp and the gate goes
    blind. This pins the coupling so a future "trim the nightly" cannot quietly
    re-break the gate by lowering n alone.
    """
    lo, hi = g.fool_rate_ci(50.0, g.DEFAULT_MAX_TRIALS)
    assert (hi - lo) / 2 <= g.MAX_CI_HALF_WIDTH_PP, \
        "DEFAULT_MAX_TRIALS too small to certify a PASS at its own power gate"


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


# NOTE: the __main__ block MUST stay at the very END of this file. _run() reads
# globals() at CALL time, so any test defined below the call is not yet bound
# and is silently never collected. That was live until 2026-09-20: the block sat
# here, mid-file, and the ten provenance / write_proxy_half tests below it never
# ran once while the file printed a confident "10/13 passed". Those are exactly
# the tests guarding ProvenanceRefusal -- the check that stops an unattributable
# verdict from being written. A test runner that reports success for tests it
# never executed is .claude/rules/reports-success-does-nothing.md in the test
# harness itself. Add new tests anywhere above the block; never below it.


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


if __name__ == "__main__":
    _run()
