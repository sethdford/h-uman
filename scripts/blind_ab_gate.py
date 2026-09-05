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


def human_is_attributable(human):
    """True when the human record carries proof of who produced it.

    write_human_half() ALWAYS stamps `tool`. A human block without it was
    written by something other than the sanctioned writer, so nothing can
    vouch for its origin — and this key is promotion-authoritative (the C LoRA
    gate reads only "human").

    Observed 2026-07-27: the live gate's human half read
    {verdict PASS, detection 0.225, n 40} with NO `tool` and no `ci_hi`, while
    its underlying sheet split exactly 20 A / 20 B with zero confidence values
    on all 40 rows — the shape of a programmatic fill, not human rating. There
    is precedent: on 2026-07-26 a synthetic run replaced a genuine n=12 human
    verdict with an n=160 machine one, which is why write_synthetic_half()
    exists as a separate key.

    An unstamped PASS is not evidence. Treat it as ABSENT rather than trust it
    — the same fail-closed posture the Binoculars base guard takes, and what
    .claude/rules/no-number-without-a-measurement.md requires: a stage that
    cannot substantiate a verdict must not emit one.
    """
    return bool((human or {}).get("tool"))


def compute_effective_verdict(proxy, human):
    """Merge proxy + human verdicts. Human FAIL is an absolute veto.

    Attribution is applied ASYMMETRICALLY, and the asymmetry is the point —
    both directions fail closed toward NOT promoting:

      unstamped PASS -> ABSENT   never grant promotion on unverifiable evidence
      unstamped FAIL -> FAIL     never UNBLOCK on unverifiable evidence

    Downgrading an unstamped FAIL would be the unsafe direction: it would let
    anyone erase a legitimate veto simply by writing a record the sanctioned
    writer never produced. A symmetric rule ("unverifiable means absent") reads
    tidier but hands out exactly that erasure — caught by
    test_effective_human_fail_vetoes_proxy_pass when this was first written
    symmetrically.

    Returns 'PASS' | 'FAIL' | 'ADVISORY'.
    """
    hv = (human or {}).get("verdict", "ABSENT")
    if hv == "PASS" and not human_is_attributable(human):
        hv = "ABSENT"
    pv = (proxy or {}).get("verdict", "ABSENT")
    pmode = (proxy or {}).get("mode", "ADVISORY")
    if hv == "FAIL":
        return "FAIL"
    if hv == "PASS" and pv == "PASS":
        return "PASS"
    if pmode == "ENFORCING" and pv in ("PASS", "FAIL"):
        return pv
    return "ADVISORY"


class ProvenanceRefusal(ValueError):
    """Raised BEFORE the gate file is touched when a proxy verdict cannot be
    attributed to what was actually serving. The gate stays at its last real
    measurement (no number beats a fabricated one)."""


def proxy_provenance_refusal(serving, claims_adapter=True):
    """Why a proxy verdict must NOT be written, or None when it may.

    `serving` is the dict gen_classifier_trials.serving_provenance() returns
    (adapter_path, tensors_loaded, adapter_bound, provenance_available, ...),
    captured from :8741 at verdict time. `claims_adapter` is whether the run
    is meant to measure the adapter arm (the default; --base-arm clears it).

    2026-07-26 -> 09-04 the server named an adapter and bound 0 tensors while
    /health said adapter_applied:true; every proxy number in that window
    measured base+prompt and the gate's provenance was annotated by hand
    afterwards. The rules below are the ways that lie can recur:

      adapter named, tensors_loaded == 0       -> refuse (either arm)
      no provenance at all (None)              -> refuse when adapter claimed
      endpoints unreachable                    -> refuse when adapter claimed
      server reports no adapter / unknown bind -> refuse when adapter claimed
    """
    if serving is None:
        if not claims_adapter:
            return None
        return ("no serving provenance captured; a verdict that cannot say what "
                "generated its replies is an annotation, not a measurement")
    adapter = serving.get("adapter_path")
    tensors = serving.get("tensors_loaded")
    if adapter and isinstance(tensors, int) and tensors == 0:
        return (f"server {serving.get('server')} names adapter {adapter} but reports "
                f"tensors_loaded=0 (nothing bound); replies measured the base model "
                f"under an adapter label")
    if not claims_adapter:
        return None
    if not serving.get("provenance_available"):
        return (f"provenance endpoints unreachable at {serving.get('server')} and the run "
                f"claims the adapter arm; cannot attribute the verdict (pass --base-arm "
                f"only if a raw-base measurement is intended)")
    if not adapter:
        return ("server reports no adapter loaded but the run claims the adapter arm; "
                "pass --base-arm to measure the base on purpose")
    if serving.get("adapter_bound") is not True:
        return (f"server names adapter {adapter} but does not report tensors_loaded; "
                f"binding is unverifiable, so the adapter arm cannot be attributed")
    return None


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


def write_proxy_half(path, proxy_fields, commit=None, serving=None, claims_adapter=True):
    """Write the proxy half. Raises ProvenanceRefusal, with the file untouched,
    unless `serving` (see proxy_provenance_refusal) can vouch for the arm the
    run claims. Every caller must capture provenance at verdict time; there is
    no way to write an adapter-arm verdict without it."""
    reason = proxy_provenance_refusal(serving, claims_adapter)
    if reason:
        raise ProvenanceRefusal(reason)
    data = _load(path)
    proxy = {"tool": "eval_blinded_ab.py",
             "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S")}
    proxy.update(proxy_fields)
    proxy["claims_adapter"] = bool(claims_adapter)
    proxy["serving"] = serving
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
