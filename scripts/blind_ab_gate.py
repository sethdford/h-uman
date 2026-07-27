#!/usr/bin/env python3
"""Blind-A/B measurement gate: single source of truth + verdict logic.

Pure stdlib. Owns the gate JSON schema, the effective-verdict truth table,
and the Tier-1 proxy decision. eval_blinded_ab.py (proxy) and
blind_ab/score.py (human) write their halves through write_*_half().
"""
import json
import os
import time

SCHEMA_VERSION = 1
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GATE_PATH = os.path.join(_REPO_ROOT, "docs", "evaluation", "blind_ab_gate.json")

ENFORCE_MIN_PAIRS = 30
DEFAULT_FAIL_UNDER = 45.0
DEFAULT_MAX_REGRESSION = 5.0
HUMAN_STALE_DAYS = 30


def proxy_gate_decision(fool_rate, n_real_pairs, baseline,
                        fail_under=DEFAULT_FAIL_UNDER,
                        max_regression=DEFAULT_MAX_REGRESSION,
                        enforce_min_pairs=ENFORCE_MIN_PAIRS):
    """Return (mode, verdict, should_fail).

    mode: 'ENFORCING' if enough real pairs else 'ADVISORY'.
    verdict: 'PASS'|'FAIL' when ENFORCING, else 'ADVISORY'.
    should_fail: True only when ENFORCING and (below floor OR regressed).
    """
    if n_real_pairs < enforce_min_pairs:
        return "ADVISORY", "ADVISORY", False
    below_floor = fool_rate < fail_under
    regressed = (
        baseline is not None
        and baseline.get("fool_rate") is not None
        and (baseline["fool_rate"] - fool_rate) > max_regression
    )
    if below_floor or regressed:
        return "ENFORCING", "FAIL", True
    return "ENFORCING", "PASS", False


def compute_effective_verdict(proxy, human):
    """Merge proxy + human verdicts. Human FAIL is an absolute veto.

    Returns 'PASS' | 'FAIL' | 'ADVISORY'.
    """
    hv = (human or {}).get("verdict", "ABSENT")
    pv = (proxy or {}).get("verdict", "ABSENT")
    pmode = (proxy or {}).get("mode", "ADVISORY")
    if hv == "FAIL":
        return "FAIL"
    if hv == "PASS" and pv == "PASS":
        return "PASS"
    if pmode == "ENFORCING" and pv in ("PASS", "FAIL"):
        return pv
    return "ADVISORY"


def _load(path):
    if os.path.exists(path):
        try:
            with open(path) as f:
                return json.load(f)
        except (ValueError, OSError):
            pass
    return {
        "schema_version": SCHEMA_VERSION, "commit": None,
        "proxy": {"verdict": "ABSENT", "mode": "ADVISORY"},
        "human": {"verdict": "ABSENT"},
        "effective_verdict": "ADVISORY",
    }


def _save(path, data):
    data["effective_verdict"] = compute_effective_verdict(
        data.get("proxy"), data.get("human"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def write_proxy_half(path, proxy_fields, commit=None):
    data = _load(path)
    proxy = {"tool": "eval_blinded_ab.py",
             "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S")}
    proxy.update(proxy_fields)
    data["proxy"] = proxy
    if commit:
        data["commit"] = commit
    _save(path, data)
    return data


def write_human_half(path, human_fields):
    data = _load(path)
    human = {"tool": "blind_ab/score.py",
             "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S")}
    human.update(human_fields)
    data["human"] = human
    _save(path, data)
    return data


def write_synthetic_half(path, synthetic_fields):
    """Record a machine-rater (synthetic-judge) scoring run.

    Deliberately a separate key from "human" and "proxy": a synthetic
    2AFC run must never masquerade as human evidence (2026-07-26: one
    did, replacing the genuine n=12 human verdict with an n=160 machine
    one), and it is not the nightly Binoculars proxy either.
    compute_effective_verdict ignores this key — synthetic results are
    observability, never promotion input.
    """
    data = _load(path)
    synthetic = {"tool": "blind_ab/score.py (synthetic rater)",
                 "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S")}
    synthetic.update(synthetic_fields)
    data["synthetic"] = synthetic
    _save(path, data)
    return data
