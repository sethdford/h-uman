---
title: Blind-A/B Measurement Gate — Implementation Plan
description: Six-task TDD plan to convert the inert blind-A/B harnesses into an enforced two-tier measurement gate (LLM-judge proxy + human veto + fail-closed capability registry).
date: 2026-05-31
status: implemented
---

# Blind-A/B Measurement Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert h-uman's inert blind-A/B harnesses into an enforced two-tier measurement gate (automated LLM-judge proxy + authoritative human veto + a declarative capability registry that CI checks fail-closed), so `feature-gate-requires-measurement` becomes real instead of theater.

**Architecture:** A single pure-Python core (`scripts/blind_ab_gate.py`) owns the gate JSON schema, the `compute_effective_verdict()` truth table, and the proxy decision. `eval_blinded_ab.py --gate` (Tier-1) and `blind_ab/score.py --emit-gate` (Tier-2) write their halves through it. A registry (`docs/evaluation/capability_gates.json`) lists each gated capability; `scripts/check_capability_gates.py` fails CI closed if any capability is LIVE without a green gate. No capability is flipped LIVE by this plan.

**Tech Stack:** Python 3 stdlib only (no pytest/pyyaml dependency — matches h-uman's `test_*.py` script convention); GitHub Actions YAML; existing Gemini-judge + human-rater harnesses.

**Spec:** `docs/superpowers/specs/2026-05-31-blind-ab-measurement-gate-design.md`
**Spec amendment:** registry is JSON (`capability_gates.json`), not YAML, to avoid a `pyyaml` CI dependency. All other spec details unchanged.

---

## File Structure

| File | Responsibility | Action |
|---|---|---|
| `scripts/blind_ab_gate.py` | Pure core: schema, `compute_effective_verdict`, `proxy_gate_decision`, read/merge/write gate JSON | Create |
| `scripts/test_blind_ab_gate.py` | Unit tests (truth table, proxy decision, merge) — stdlib runner | Create |
| `scripts/eval_blinded_ab.py` | Tier-1 LLM-judge; add `--gate` mode + exit code | Modify |
| `scripts/blind_ab/score.py` | Tier-2 human scoring; add `--emit-gate <path>` | Modify |
| `docs/evaluation/capability_gates.json` | Declarative registry of gated capabilities (seeded to current reality) | Create |
| `scripts/check_capability_gates.py` | CI registry check — fail-closed if LIVE without green gate | Create |
| `scripts/test_check_capability_gates.py` | Unit tests for the registry check | Create |
| `docs/evaluation/blind_ab_gate.json` | Single source of truth (generated; seeded ABSENT/ADVISORY) | Create (seed) |
| `.github/workflows/evaluation.yml` | Add `blind-ab-gate` job + registry-check step | Modify |

---

## Task 1: Gate core module (`scripts/blind_ab_gate.py`)

**Files:**
- Create: `scripts/blind_ab_gate.py`
- Test: `scripts/test_blind_ab_gate.py`

- [ ] **Step 1: Write the failing test**

Create `scripts/test_blind_ab_gate.py`:

```python
#!/usr/bin/env python3
"""Unit tests for scripts/blind_ab_gate.py (stdlib runner — no pytest dep)."""
import os, sys, json, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blind_ab_gate as g


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
    # proxy advisory + stale human -> advisory (stale != PASS)
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
        g.write_proxy_half(p, {"fool_rate": 50.0, "mode": "ENFORCING",
                               "verdict": "PASS", "n_real_pairs": 40,
                               "n_trials": 40, "baseline_fool_rate": None,
                               "fail_under": 45, "max_regression": 5}, commit="abc")
        g.write_human_half(p, {"detection": 0.5, "ci_lo": 0.4, "n": 30,
                               "verdict": "PASS"})
        data = json.load(open(p))
        assert data["proxy"]["fool_rate"] == 50.0      # preserved
        assert data["human"]["verdict"] == "PASS"      # merged
        assert data["effective_verdict"] == "PASS"     # recomputed


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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 scripts/test_blind_ab_gate.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'blind_ab_gate'`

- [ ] **Step 3: Write minimal implementation**

Create `scripts/blind_ab_gate.py`:

```python
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
# docs/evaluation/blind_ab_gate.json relative to repo root (this file is in scripts/)
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GATE_PATH = os.path.join(_REPO_ROOT, "docs", "evaluation", "blind_ab_gate.json")

ENFORCE_MIN_PAIRS = 30      # below this many real pairs the proxy is ADVISORY
DEFAULT_FAIL_UNDER = 45.0   # fool-rate floor (%), matches eval_blinded_ab "target"
DEFAULT_MAX_REGRESSION = 5.0  # max allowed fool-rate drop vs baseline (pts)
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
        return "FAIL"                       # human veto
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 scripts/test_blind_ab_gate.py`
Expected: `10/10 passed`, exit 0

- [ ] **Step 5: Commit**

```bash
git add scripts/blind_ab_gate.py scripts/test_blind_ab_gate.py
git commit -m "feat(blind-ab): gate core — verdict truth table + proxy decision"
```

---

## Task 2: Seed the gate JSON + wire `eval_blinded_ab.py --gate`

**Files:**
- Create: `docs/evaluation/blind_ab_gate.json`
- Modify: `scripts/eval_blinded_ab.py` (flag parsing block `:62-69`; results/exit block `:376-388`)

- [ ] **Step 1: Seed the gate file (ABSENT/ADVISORY initial state)**

Create `docs/evaluation/blind_ab_gate.json`:

```json
{
  "schema_version": 1,
  "commit": null,
  "proxy": {
    "tool": "eval_blinded_ab.py",
    "verdict": "ABSENT",
    "mode": "ADVISORY",
    "fool_rate": null,
    "baseline_fool_rate": null,
    "n_trials": 0,
    "n_real_pairs": 0,
    "fail_under": 45,
    "max_regression": 5,
    "timestamp": null
  },
  "human": {
    "tool": "blind_ab/score.py",
    "verdict": "ABSENT",
    "detection": null,
    "ci_lo": null,
    "n": 0,
    "timestamp": null
  },
  "effective_verdict": "ADVISORY"
}
```

- [ ] **Step 2: Write the failing test (subprocess smoke — ADVISORY on synthetic)**

Append to `scripts/test_blind_ab_gate.py` before `_run()`:

```python
def test_eval_gate_synthetic_is_advisory_and_exits_zero():
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as d:
        gate = os.path.join(d, "gate.json")
        # --gate with no creds/real pairs must NOT hard-fail: ADVISORY, exit 0.
        env = dict(os.environ, HU_BLIND_AB_GATE_PATH=gate)
        r = subprocess.run(
            ["python3", os.path.join(here, "eval_blinded_ab.py"),
             "--gate", "--gate-dry-run"],
            capture_output=True, text=True, env=env, timeout=60)
        assert r.returncode == 0, r.stderr
        data = json.load(open(gate))
        assert data["proxy"]["mode"] == "ADVISORY"
        assert data["effective_verdict"] == "ADVISORY"
```

Note: `--gate-dry-run` (added in Step 3) skips live judge calls and writes an
ADVISORY proxy half from `n_real_pairs=0` — lets CI/tests exercise gate wiring
without Gemini creds.

- [ ] **Step 3: Run test to verify it fails**

Run: `python3 scripts/test_blind_ab_gate.py`
Expected: FAIL — `test_eval_gate_synthetic...` errors (flags unknown; no gate written)

- [ ] **Step 4: Implement `--gate` in `eval_blinded_ab.py`**

After the flag block (`scripts/eval_blinded_ab.py:62-69`), add:

```python
USE_GATE = "--gate" in sys.argv
GATE_DRY_RUN = "--gate-dry-run" in sys.argv
import blind_ab_gate as _gate
_GATE_PATH = os.environ.get("HU_BLIND_AB_GATE_PATH", _gate.GATE_PATH)


def _git_commit():
    import subprocess
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True).strip()
    except Exception:
        return None


def _load_baseline():
    try:
        with open(_GATE_PATH) as f:
            return {"fool_rate": json.load(f)["proxy"].get("fool_rate")}
    except Exception:
        return None
```

Then, in `main()`, replace the final results/exit region
(`scripts/eval_blinded_ab.py:376-388`, the `os.makedirs(...RESULTS_PATH...)`
block) so that **after** writing `RESULTS_PATH` as today, it also handles the
gate. Insert before the existing `os.makedirs` line a dry-run short-circuit,
and after the existing JSON dump add the gate write + exit:

```python
    # --- gate dry run: no live judging; emit ADVISORY proxy half, exit 0 ---
    if USE_GATE and GATE_DRY_RUN:
        _gate.write_proxy_half(_GATE_PATH, {
            "verdict": "ADVISORY", "mode": "ADVISORY", "fool_rate": None,
            "baseline_fool_rate": None, "n_trials": 0, "n_real_pairs": 0,
            "fail_under": _gate.DEFAULT_FAIL_UNDER,
            "max_regression": _gate.DEFAULT_MAX_REGRESSION,
        }, commit=_git_commit())
        print("GATE: ADVISORY (dry run / no data) — not blocking")
        sys.exit(0)
```

(Keep the existing `RESULTS_PATH` dump unchanged.) Immediately after that dump,
add:

```python
    if USE_GATE:
        n_real_pairs = sum(1 for t in results if not t.get("is_synthetic"))
        baseline = _load_baseline()
        mode, verdict, should_fail = _gate.proxy_gate_decision(
            fool_rate=fool_rate if total else 0.0,
            n_real_pairs=n_real_pairs, baseline=baseline)
        _gate.write_proxy_half(_GATE_PATH, {
            "verdict": verdict, "mode": mode,
            "fool_rate": fool_rate if total else None,
            "baseline_fool_rate": (baseline or {}).get("fool_rate"),
            "n_trials": total, "n_real_pairs": n_real_pairs,
            "fail_under": _gate.DEFAULT_FAIL_UNDER,
            "max_regression": _gate.DEFAULT_MAX_REGRESSION,
        }, commit=_git_commit())
        banner = ("ADVISORY (n_real_pairs<%d) — not blocking" % _gate.ENFORCE_MIN_PAIRS
                  if mode == "ADVISORY" else "%s (fool_rate=%.0f%%)" % (verdict, fool_rate))
        print(f"\n  GATE: {banner}")
        sys.exit(1 if should_fail else 0)
```

Guard: the dry-run block references `fool_rate`/`results`/`total` only in the
non-dry-run path, so place the dry-run short-circuit **before** the trials loop
is required — simplest is to put it at the very top of `main()` right after the
creds check. Move the dry-run block accordingly (it needs no pairs).

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 scripts/test_blind_ab_gate.py`
Expected: `11/11 passed`, exit 0

- [ ] **Step 6: Commit**

```bash
git add scripts/eval_blinded_ab.py docs/evaluation/blind_ab_gate.json scripts/test_blind_ab_gate.py
git commit -m "feat(blind-ab): --gate mode on eval_blinded_ab.py (ADVISORY-safe, fail-closed when enforcing)"
```

---

## Task 3: Wire `blind_ab/score.py --emit-gate`

**Files:**
- Modify: `scripts/blind_ab/score.py` (argparse + after `score_rows` aggregation)
- Test: extend `scripts/test_blind_ab_gate.py`

- [ ] **Step 1: Write the failing test**

Append to `scripts/test_blind_ab_gate.py` before `_run()`:

```python
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
            for i in range(4):
                w.writerow([str(i), "A", "4"])
        json.dump({str(i): "A" for i in range(4)}, open(keyf, "w"))
        r = subprocess.run(
            ["python3", score, sheet, "--key", keyf, "--emit-gate", gate],
            capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, r.stderr
        data = json.load(open(gate))
        assert data["human"]["verdict"] in ("PASS", "FAIL")
        assert data["human"]["n"] == 4
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 scripts/test_blind_ab_gate.py`
Expected: FAIL — `--emit-gate` unrecognized / no gate written.

- [ ] **Step 3: Implement `--emit-gate`**

In `scripts/blind_ab/score.py`: add the arg to its argparse parser:

```python
    ap.add_argument("--emit-gate", default=None,
                    help="Write the human half of the blind_ab gate JSON to this path")
```

After the aggregate `agg` is computed and PASS/FAIL is determined (the existing
criteria `detect <= 0.60 and ci_lo <= 0.55`), add:

```python
    if args.emit_gate:
        import os, sys
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
        import blind_ab_gate as _gate
        verdict = "PASS" if (agg["detect"] <= 0.60 and agg["ci_lo"] <= 0.55) else "FAIL"
        _gate.write_human_half(args.emit_gate, {
            "detection": round(agg["detect"], 4),
            "ci_lo": round(agg["ci_lo"], 4),
            "ci_hi": round(agg["ci_hi"], 4),
            "n": agg["n"],
            "verdict": verdict,
        })
        print(f"\nWrote human gate half ({verdict}) to {args.emit_gate}")
```

(If `score.py`'s PASS/FAIL is computed in a helper rather than inline, call that
helper here instead of re-deriving — keep one source of the criteria.)

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 scripts/test_blind_ab_gate.py`
Expected: `12/12 passed`, exit 0

- [ ] **Step 5: Commit**

```bash
git add scripts/blind_ab/score.py scripts/test_blind_ab_gate.py
git commit -m "feat(blind-ab): score.py --emit-gate writes authoritative human half"
```

---

## Task 4: Capability registry + fail-closed CI check

**Files:**
- Create: `docs/evaluation/capability_gates.json`
- Create: `scripts/check_capability_gates.py`
- Test: `scripts/test_check_capability_gates.py`

- [ ] **Step 1: Create the registry seeded to CURRENT reality (nothing newly LIVE)**

Create `docs/evaluation/capability_gates.json`:

```json
{
  "schema_version": 1,
  "comment": "Flipping a capability `state` to LIVE is the reviewable action the CI registry check gates against docs/evaluation/blind_ab_gate.json. Seeded to current reality; this file changes no runtime behavior.",
  "capabilities": [
    {"id": "graph_grounding",  "env": "HU_GRAPH_GROUNDING", "state": "OFF",    "required_gate": "pass"},
    {"id": "salience",         "env": "HU_SALIENCE_SHADOW",  "state": "SHADOW", "required_gate": "pass"},
    {"id": "theory_of_mind",   "env": "HU_TOM",              "state": "OFF",    "required_gate": "pass"},
    {"id": "intent_response",  "env": "HU_INTENT_RESPONSE",  "state": "OFF",    "required_gate": "pass"}
  ]
}
```

- [ ] **Step 2: Write the failing test**

Create `scripts/test_check_capability_gates.py`:

```python
#!/usr/bin/env python3
"""Unit tests for scripts/check_capability_gates.py (stdlib runner)."""
import os, sys, json, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_capability_gates as c


def _write(d, registry, gate):
    rp = os.path.join(d, "reg.json"); gp = os.path.join(d, "gate.json")
    json.dump(registry, open(rp, "w")); json.dump(gate, open(gp, "w"))
    return rp, gp


def test_live_with_red_gate_fails():
    with tempfile.TemporaryDirectory() as d:
        rp, gp = _write(d,
            {"capabilities": [{"id": "x", "env": "X", "state": "LIVE", "required_gate": "pass"}]},
            {"effective_verdict": "ADVISORY"})
        assert c.check(rp, gp) != 0


def test_live_with_green_gate_passes():
    with tempfile.TemporaryDirectory() as d:
        rp, gp = _write(d,
            {"capabilities": [{"id": "x", "env": "X", "state": "LIVE", "required_gate": "pass"}]},
            {"effective_verdict": "PASS"})
        assert c.check(rp, gp) == 0


def test_non_live_with_red_gate_passes():
    with tempfile.TemporaryDirectory() as d:
        rp, gp = _write(d,
            {"capabilities": [{"id": "x", "env": "X", "state": "SHADOW", "required_gate": "pass"}]},
            {"effective_verdict": "FAIL"})
        assert c.check(rp, gp) == 0


def test_live_with_missing_gate_fails_closed():
    with tempfile.TemporaryDirectory() as d:
        rp = os.path.join(d, "reg.json")
        json.dump({"capabilities": [{"id": "x", "env": "X", "state": "LIVE", "required_gate": "pass"}]}, open(rp, "w"))
        assert c.check(rp, os.path.join(d, "does_not_exist.json")) != 0


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
```

- [ ] **Step 3: Run test to verify it fails**

Run: `python3 scripts/test_check_capability_gates.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'check_capability_gates'`

- [ ] **Step 4: Implement the check**

Create `scripts/check_capability_gates.py`:

```python
#!/usr/bin/env python3
"""CI registry check: any capability LIVE must have a green blind_ab gate.

Fail-closed: a LIVE capability with a missing/non-PASS gate fails CI. This is
the enforcement that makes feature-gate-requires-measurement real.
"""
import json
import os
import sys

_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_REGISTRY = os.path.join(_ROOT, "docs", "evaluation", "capability_gates.json")
DEFAULT_GATE = os.path.join(_ROOT, "docs", "evaluation", "blind_ab_gate.json")


def check(registry_path=DEFAULT_REGISTRY, gate_path=DEFAULT_GATE):
    """Return 0 if all LIVE capabilities have a PASS gate, else nonzero."""
    try:
        reg = json.load(open(registry_path))
    except (OSError, ValueError) as e:
        print(f"ERROR: cannot read registry {registry_path}: {e}")
        return 2
    live = [cap for cap in reg.get("capabilities", [])
            if cap.get("state") == "LIVE" and cap.get("required_gate") == "pass"]
    if not live:
        print("Capability gate check: no LIVE capabilities require the gate — OK")
        return 0
    try:
        gate = json.load(open(gate_path))
        effective = gate.get("effective_verdict")
    except (OSError, ValueError):
        effective = None  # fail closed
    if effective != "PASS":
        for cap in live:
            print(f"FAIL: capability '{cap['id']}' is LIVE but blind_ab gate "
                  f"effective_verdict={effective!r} (need PASS)")
        return 1
    print(f"Capability gate check: {len(live)} LIVE capabilities, gate PASS — OK")
    return 0


if __name__ == "__main__":
    sys.exit(check())
```

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 scripts/test_check_capability_gates.py`
Expected: `4/4 passed`, exit 0

- [ ] **Step 6: Run the real check (seeded reality → OK, nothing LIVE)**

Run: `python3 scripts/check_capability_gates.py`
Expected: `no LIVE capabilities require the gate — OK`, exit 0

- [ ] **Step 7: Commit**

```bash
git add docs/evaluation/capability_gates.json scripts/check_capability_gates.py scripts/test_check_capability_gates.py
git commit -m "feat(blind-ab): capability registry + fail-closed CI gate check"
```

---

## Task 5: CI wiring in `evaluation.yml`

**Files:**
- Modify: `.github/workflows/evaluation.yml` (add a job; add a no-creds check step)

- [ ] **Step 1: Add the registry check as a standalone no-creds job**

Add to `.github/workflows/evaluation.yml` under `jobs:` (mirror the existing
`unit-tests` job style):

```yaml
  capability-gate-check:
    name: Capability gate (fail-closed)
    runs-on: ubuntu-latest
    permissions:
      contents: read
    steps:
      - uses: actions/checkout@v6
      - name: Gate-core unit tests
        run: |
          python3 scripts/test_blind_ab_gate.py
          python3 scripts/test_check_capability_gates.py
      - name: Enforce capability registry against blind_ab gate
        run: python3 scripts/check_capability_gates.py
```

- [ ] **Step 2: Add the Tier-1 proxy job (creds-gated, ADVISORY-safe)**

Add (mirror `frontier-live`'s gated pattern — runs the proxy, ADVISORY without
real data so it never falsely blocks):

```yaml
  blind-ab-gate:
    name: Blind-A/B proxy gate (LLM-judge)
    runs-on: ubuntu-latest
    permissions:
      contents: read
    steps:
      - uses: actions/checkout@v6
      - name: Run blind-A/B proxy in gate mode
        env:
          GEMINI_API_KEY: ${{ secrets.GEMINI_API_KEY }}
        run: |
          if [ -z "$GEMINI_API_KEY" ]; then
            echo "No GEMINI_API_KEY — running gate dry-run (ADVISORY)"
            python3 scripts/eval_blinded_ab.py --gate --gate-dry-run
          else
            python3 scripts/eval_blinded_ab.py --gate || EXIT=$?
            python3 -c "import json;d=json.load(open('docs/evaluation/blind_ab_gate.json'));print('GATE',d['effective_verdict'],d['proxy']['mode'])"
            exit ${EXIT:-0}
          fi
      - name: Upload gate report
        if: always()
        uses: actions/upload-artifact@v7
        with:
          name: blind-ab-gate
          path: docs/evaluation/blind_ab_gate.json
```

- [ ] **Step 3: Validate the workflow YAML locally**

Run: `python3 -c "import sys,json; print('yaml lib optional'); print(open('.github/workflows/evaluation.yml').read()[:1])"`
Then if `yamllint` or `actionlint` is available: `actionlint .github/workflows/evaluation.yml`
Expected: no syntax errors. (If neither tool exists, visually confirm indentation matches sibling jobs.)

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/evaluation.yml
git commit -m "ci(blind-ab): wire proxy gate job + fail-closed capability registry check"
```

---

## Task 6: Non-vacuous proof + real-pairs follow-up

**Files:**
- Test: temporary fixture (not committed)
- Create: `docs/evaluation/REAL_PAIRS_FOLLOWUP.md`

- [ ] **Step 1: Prove the gate is non-vacuous (can actually fail)**

Run this throwaway check (do not commit the mutated files):

```bash
# Flip a capability LIVE + red gate; assert the check FAILS.
cp docs/evaluation/capability_gates.json /tmp/reg.json
cp docs/evaluation/blind_ab_gate.json /tmp/gate.json
python3 - <<'PY'
import json
r=json.load(open('/tmp/reg.json')); r['capabilities'][0]['state']='LIVE'
json.dump(r,open('/tmp/reg.json','w'))
g=json.load(open('/tmp/gate.json')); g['effective_verdict']='ADVISORY'
json.dump(g,open('/tmp/gate.json','w'))
PY
python3 scripts/check_capability_gates.py /tmp/reg.json /tmp/gate.json; echo "exit=$? (EXPECT nonzero)"
```

Expected: prints `FAIL: capability 'graph_grounding' is LIVE ...`, `exit=1`.
(Requires `check()` to accept argv — confirm `if __name__` passes
`sys.argv[1:]`; if not, add: `sys.exit(check(*sys.argv[1:3]))`.)

- [ ] **Step 2: Add argv support to the check (if Step 1 revealed it's missing)**

In `scripts/check_capability_gates.py`, replace the `__main__` block:

```python
if __name__ == "__main__":
    args = sys.argv[1:]
    sys.exit(check(*args) if args else check())
```

Run: `python3 scripts/test_check_capability_gates.py`
Expected: `4/4 passed` (unchanged).

- [ ] **Step 3: Verify real callers exist (integration-done-contract)**

Run:
```bash
grep -rn "blind_ab_gate" scripts/eval_blinded_ab.py scripts/blind_ab/score.py | grep -v test_
grep -rn "check_capability_gates\|eval_blinded_ab.py --gate" .github/workflows/evaluation.yml
```
Expected: non-empty — the gate core has real importers and the CI yaml calls
both scripts. (If empty, the wiring is incomplete — fix before closing.)

- [ ] **Step 4: Write the real-pairs follow-up doc (ADVISORY → ENFORCING path)**

Create `docs/evaluation/REAL_PAIRS_FOLLOWUP.md`:

```markdown
# Blind-A/B gate: ADVISORY → ENFORCING

The blind-A/B proxy gate is ADVISORY until >= 30 real Seth pairs exist
(`scripts/blind_ab_gate.py:ENFORCE_MIN_PAIRS`). Until then it reports but
does not block — by design (no synthetic-data theater).

To switch it on:
1. Populate `data/imessage/ground_truth.jsonl` via
   `python3 scripts/extract_imessage_pairs.py` (>= 30 pairs).
2. Re-run `python3 scripts/eval_blinded_ab.py --gate` (needs GEMINI_API_KEY).
3. Confirm `docs/evaluation/blind_ab_gate.json` shows `proxy.mode == ENFORCING`.
4. Run >= 1 human cadence rating via scripts/blind_ab/ + `score.py --emit-gate`
   to populate the authoritative human half.

Only after ENFORCING + a human PASS should any capability in
`docs/evaluation/capability_gates.json` be flipped to LIVE.
```

- [ ] **Step 5: Commit**

```bash
git add docs/evaluation/REAL_PAIRS_FOLLOWUP.md scripts/check_capability_gates.py
git commit -m "docs(blind-ab): ADVISORY->ENFORCING runbook + argv for gate check"
```

---

## Final verification (run before declaring done)

- [ ] `python3 scripts/test_blind_ab_gate.py` → all pass
- [ ] `python3 scripts/test_check_capability_gates.py` → all pass
- [ ] `python3 scripts/check_capability_gates.py` → OK (nothing LIVE)
- [ ] `python3 scripts/eval_blinded_ab.py --gate --gate-dry-run` → ADVISORY, exit 0, gate JSON updated
- [ ] Non-vacuous proof (Task 6 Step 1) → check FAILS on LIVE+red
- [ ] `grep` shows real callers of the gate core and CI scripts
- [ ] `/verify` agent run capturing the above evidence (per quality-gates)

## Self-Review notes
- **Spec coverage:** C1→T2, C2→T3, C3→T1+T2(seed), C4→T4, C5→T5, anti-theater
  guards #1–4 → T2(ADVISORY)/T4(fail-closed)/T6(non-vacuous+grep). All covered.
- **Type consistency:** `compute_effective_verdict(proxy, human)`,
  `proxy_gate_decision(...)->(mode,verdict,should_fail)`, `write_proxy_half`,
  `write_human_half`, `check(registry_path, gate_path)` used consistently across
  tasks and tests.
- **No capability flipped LIVE** anywhere in this plan (verified: registry
  seeded all OFF/SHADOW).
