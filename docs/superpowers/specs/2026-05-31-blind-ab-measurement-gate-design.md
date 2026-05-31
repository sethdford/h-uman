---
title: Blind-A/B Measurement Gate — Design Spec
description: Two-tier (LLM-judge proxy + authoritative human veto) measurement gate that makes feature-gate-requires-measurement enforceable instead of theater.
date: 2026-05-31
status: approved
---

# Blind-A/B Measurement Gate — Design Spec

**Date:** 2026-05-31
**Status:** Approved (design); pending spec review
**Branch:** `worktree-feat+blind-ab-gate` (off `origin/main` @ 3977ca90)
**Author:** keystone-first session

## Problem

h-uman's `feature-gate-requires-measurement` discipline says a subsystem may
go LIVE only after a measurement proves it does not regress humanness. The
*only* ground-truth humanness signal is the blind-A/B ("indistinguishable from
Seth") measurement. Today that signal:

- is **not enforced in CI** — `evaluation.yml` gates only the single-turn
  `HU_EVAL_THRESHOLD_TURING` proxy, never the blind-A/B fool-rate;
- **cannot gate anything even when run** — `scripts/eval_blinded_ab.py`
  computes a `fool_rate` and a VERDICT, writes a JSON report, then **always
  exits 0** regardless of result (it only `sys.exit(1)` on missing
  creds/data). It is a *reporter*, not a *gate*.
- is otherwise **human-rater-gated and manual** (`scripts/blind_ab/`), with
  ~0–2 real results across 11+ sprints.

Net effect: the gate rule is theater. Untested behavior changes can ship LIVE
with zero automated gate and zero rollback signal. Every downstream activation
(GraphRAG grounding, salience, ToM, intent classifier, …) is blocked from
*safe* rollout because there is no measurement to gate on.

This spec makes the blind-A/B an **enforced, two-tier gate**. It does NOT flip
any capability LIVE — that is downstream work this gate unblocks.

## What already exists (this is wire-and-gate, not build)

| Asset | Role | Current state |
|---|---|---|
| `scripts/eval_blinded_ab.py` | Tier-1 LLM-judge (Gemini 3.1 Pro), emits `fool_rate`, has `--gateway` live mode | Runs, writes JSON, **always exits 0** |
| `scripts/blind_ab/score.py` | Tier-2 human rater scoring: detection rate + 95% Wilson CI + PASS/FAIL (`detect ≤ 0.60 AND ci_lo ≤ 0.55`) | Prints verdict; **emits no machine-readable file** |
| `scripts/extract_imessage_pairs.py` | Produces real Seth ground-truth pairs | Exists; pairs corpus ≈ 0 |
| `.github/workflows/evaluation.yml` | CI eval; "Verify per-suite regression thresholds" step; `frontier-live` (live, gated, needs-creds) job; baseline-update-PR mechanism | Pattern to copy; no blind-A/B gate |

## Goals / Non-Goals

**Goals**
1. A single machine-readable source of truth for the blind-A/B verdict.
2. Tier-1 (LLM-judge) enforced automatically in CI with an absolute floor and
   a regression-vs-baseline check, emitting a rollback signal (nonzero exit).
3. Tier-2 (human) emits the same verdict shape on a cadence and is
   **authoritative** — a human FAIL vetoes a Tier-1 PASS.
4. A declarative capability registry such that flipping a capability to LIVE
   is a reviewable edit that CI checks against the gate.
5. Honesty guards so a synthetic-data gate cannot masquerade as ground truth.

**Non-Goals (explicitly out of scope this session)**
- Flipping GraphRAG / salience / ToM / intent / any capability LIVE.
- Calibrated ensemble judge (rejected: ~2 human points cannot calibrate).
- Touching the six in-flight feature branches (they *consume* this gate later).
- Building any new judge or new measurement — only wiring existing ones.

## Architecture

```
Tier 1 (proxy, automated)         Tier 2 (truth, cadence)          Gate (teeth)
eval_blinded_ab.py --gate  ──►    blind_ab/score.py --emit-gate ──► docs/evaluation/
  LLM-judge fool_rate               human detection + Wilson CI       blind_ab_gate.json
        │                                   │                               ▲  (single source of truth)
        └── CI job (sibling of frontier-live) ──────────────────────────────┤
                                                                            │
              docs/evaluation/capability_gates.yml ── CI registry check ────┘
              (each HU_* capability: state OFF|SHADOW|LIVE + required_gate)
```

## Components

### C1. `eval_blinded_ab.py` — add gate mode (extend, do not rewrite)
- New flags: `--gate` (enable gate semantics), `--fail-under <pct>` (absolute
  fool-rate floor, default **45** — matches the file's existing "Target ≥ 45%"),
  `--baseline <path>` (regression check), `--max-regression <pct>` (default 5).
- Behaviour with `--gate`:
  - Compute `fool_rate` and `n_real_pairs` (count of non-synthetic pairs used).
  - Determine **mode**: `ENFORCING` if `n_real_pairs >= N` (N=**30**), else
    `ADVISORY`.
  - Write the gate JSON (see C3) — proxy half.
  - **Exit code:** in `ENFORCING` mode exit 1 if `fool_rate < fail_under` OR
    `(baseline.fool_rate - fool_rate) > max_regression`; else exit 0. In
    `ADVISORY` mode always exit 0 but print a loud `GATE: ADVISORY (n_real_pairs
    < N) — not blocking` banner.
- Without `--gate`: unchanged (still exit 0). No existing caller breaks.

### C2. `blind_ab/score.py` — emit machine-readable verdict (extend)
- New flag `--emit-gate <path>`: after computing `agg`, write the human half of
  the gate JSON: `detection`, `ci_lo`, `ci_hi`, `n`, `verdict` (PASS/FAIL via the
  existing `detect ≤ 0.60 AND ci_lo ≤ 0.55` criteria), `timestamp`.
- Printed output unchanged.

### C3. `docs/evaluation/blind_ab_gate.json` (new) — single source of truth
Schema:
```json
{
  "schema_version": 1,
  "commit": "<git sha the proxy ran against>",
  "proxy": {
    "tool": "eval_blinded_ab.py",
    "fool_rate": 0.0, "baseline_fool_rate": null,
    "n_trials": 0, "n_real_pairs": 0,
    "mode": "ADVISORY|ENFORCING",
    "fail_under": 45, "max_regression": 5,
    "verdict": "PASS|FAIL|ADVISORY",
    "timestamp": "ISO8601"
  },
  "human": {
    "tool": "blind_ab/score.py",
    "detection": null, "ci_lo": null, "n": 0,
    "verdict": "PASS|FAIL|STALE|ABSENT",
    "timestamp": null
  },
  "effective_verdict": "PASS|FAIL|ADVISORY"
}
```
- `effective_verdict` rule: **human FAIL ⇒ FAIL** (veto). Else if human PASS
  and proxy PASS ⇒ PASS. Else if proxy ENFORCING ⇒ proxy verdict. Else
  ⇒ ADVISORY. A small pure function `compute_effective_verdict()` owns this and
  is unit-tested.
- Human half is `ABSENT`/`STALE` until real human runs exist; `STALE` if the
  human timestamp is older than a configurable window (default 30 days).

### C4. `docs/evaluation/capability_gates.yml` (new) — declarative registry
```yaml
# Each gated capability. Flipping `state` to LIVE is the reviewable action
# that the CI registry check gates against blind_ab_gate.json.
capabilities:
  - id: graph_grounding
    env: HU_GRAPH_GROUNDING
    state: OFF          # OFF | SHADOW | LIVE
    required_gate: pass # gate effective_verdict must be PASS to be LIVE
  - id: salience
    env: HU_SALIENCE_SHADOW
    state: SHADOW
    required_gate: pass
  - id: theory_of_mind
    env: HU_TOM
    state: OFF
    required_gate: pass
  # … (intent classifier, etc. — seeded from current reality, all non-LIVE)
```
- The registry is seeded to reflect **current** reality (nothing is LIVE that
  is not already LIVE). This spec changes no state.

### C5. CI wiring (`evaluation.yml` sibling job + registry check)
- **Job `blind-ab-gate`** (sibling of `frontier-live`, gated on the same creds
  pattern; uses `--synthetic` only as a labelled fallback that forces ADVISORY):
  runs `eval_blinded_ab.py --gate --baseline docs/evaluation/blind_ab_gate.json`,
  uploads the report artifact, and surfaces the proxy verdict. ENFORCING-mode
  failure fails the job.
- **Registry check** (cheap, no creds, runs always): a small script reads
  `capability_gates.yml`; for every capability with `state: LIVE` and
  `required_gate: pass`, assert `blind_ab_gate.json.effective_verdict == PASS`.
  Fails CI otherwise. This is the teeth that make the feature-gate rule real:
  you cannot merge a registry edit to LIVE unless the measured gate is green.

## Anti-theater guards (honesty is a requirement, not a nicety)
1. **Advisory below data threshold.** `mode = ENFORCING` only when
   `n_real_pairs >= 30`; otherwise ADVISORY and explicitly non-blocking, with a
   loud banner. Prevents a synthetic-only gate from masquerading as truth.
2. **Real-pairs dependency named, not hidden.** Populating real Seth pairs via
   `extract_imessage_pairs.py` is a tracked follow-up; `--synthetic` is a
   labelled fallback that *forces* ADVISORY, never silent substitution.
3. **Non-vacuous gate (per `integration-done-contract`).** The gate's own tests
   feed a known-bad fool-rate and assert exit 1 / CI failure, and a known-good
   and assert pass. A gate that cannot fail is not a gate.
4. **Real caller, proven by grep.** The CI job + registry check are the callers
   of the new gate code; CI yaml references the scripts. No define-and-isolate.

## Data flow
1. CI eval run → `eval_blinded_ab.py --gate` → writes `proxy` half of
   `blind_ab_gate.json` + exit code (ENFORCING).
2. Human cadence run → `blind_ab/score.py --emit-gate` → writes `human` half.
3. `compute_effective_verdict()` merges → `effective_verdict`.
4. Any PR editing `capability_gates.yml` to set a capability LIVE → registry
   check requires `effective_verdict == PASS` → pass/fail CI.

## Error handling
- Missing creds / zero pairs in CI: job runs `--synthetic` fallback → ADVISORY,
  job **passes** (does not block) but annotates "advisory: no real data/creds".
- Missing/last-run gate JSON for registry check: if any capability is LIVE and
  the gate file is absent or `effective_verdict != PASS` → **fail** (fail
  closed: LIVE without a green measurement is exactly what we forbid).
- Malformed gate JSON: registry check fails closed with a clear error.

## Testing
- **Unit (Python):** `compute_effective_verdict()` truth table (human veto,
  advisory, enforcing pass/fail); `--gate` exit-code logic on fixture JSONs
  (pass, fail-under, regression, advisory); `score.py --emit-gate` JSON matches
  printed verdict.
- **Unit (registry check):** LIVE + red gate ⇒ fail; LIVE + green ⇒ pass;
  non-LIVE + red ⇒ pass; missing gate + LIVE ⇒ fail closed.
- **Smoke:** `eval_blinded_ab.py --gate --synthetic` produces ADVISORY JSON and
  exits 0.

## Definition of Done
- `blind_ab_gate.json` schema + `compute_effective_verdict()` implemented + unit
  tested (truth table green).
- `eval_blinded_ab.py --gate` enforces floor + regression, ADVISORY below N=30,
  with tests proving it can fail.
- `score.py --emit-gate` emits the human half.
- `capability_gates.yml` seeded to current reality (nothing newly LIVE).
- CI `blind-ab-gate` job + registry check wired in `evaluation.yml`; registry
  check proven to fail on a LIVE+red fixture (non-vacuous).
- `grep` shows the new gate functions have real callers (CI yaml + registry
  script), not isolated definitions.
- Spec reviewed; follow-up task filed to populate real Seth pairs (the path
  from ADVISORY → ENFORCING).

## Risks / open items
- **Proxy ≠ truth.** LLM-judge fool-rate measures "can an LLM tell," a proxy for
  "can a human tell." Mitigated by the human veto tier; accepted as strictly
  better than today's zero-signal state.
- **Gemini creds/cost in CI.** Reuse the `frontier-live` gated pattern; the job
  no-ops to ADVISORY without creds.
- **N=30 and floor=45% are starting values**, recorded in the gate JSON so they
  are auditable and tunable as real data accrues.
