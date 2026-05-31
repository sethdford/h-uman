# Automated Humanness Measurement Loop — Requirements

> Status: DRAFT (Phase 1 — awaiting sign-off). Keystone from the 2026-05-31
> learning/resilience audit. Turns "is h-uman indistinguishable from Seth" from a
> manual, rarely-run check into an automated, gating, rollback-capable loop.

## Problem (one paragraph)

The 6 humanness gates (ToM, self-uncertainty, GraphRAG grounding, bandit
humanization, salience, intent) are **already LIVE** on the daemon via the launchd
plist env — but their only basis is a **simulated proxy that currently reads
DISTINGUISHABLE**, and the human blind-A/B rating sheet is **blank**. The
`feature-gate-requires-measurement` contract says a subsystem goes LIVE only on a
passing measurement. Today there is no automated measurement: the W16 structured
bench auto-runs and gates, but the humanness/blind-A/B path (`eval_blinded_ab.py`,
`eval_humanness.py`, the synthetic judge) runs only by hand. Result: behavior that
ships AS Seth is governed by vibes, not a signal.

## User stories

- As the platform owner, I want an automated humanness signal on every PR, so I learn *before* merge if a change made h-uman read less like me.
- As the platform owner, I want a scheduled real-judge proxy measurement against a committed baseline, so drift in the live persona is caught without me running scripts by hand.
- As the platform owner, I want a written rollback runbook tied to a failing measurement, so a bad reading flips the offending gate OFF deterministically instead of lingering.
- As the platform owner, I want the gap between the proxy and the (blank) human blind-A/B sheet made visible and escalated, so the proxy never silently becomes the unchallenged source of truth.

## Acceptance criteria

- [ ] **AC-1** — A CI workflow runs a **deterministic, secret-free** humanness regression gate on every PR (reusing the existing `shape` + persona-fidelity + turing scorers + a deterministic synthetic proxy), and **blocks merge** when the aggregate score drops below the committed baseline minus the per-metric threshold.
- [ ] **AC-2** — A **scheduled (cron) + `workflow_dispatch`** CI job runs `scripts/eval_blinded_ab.py --synthetic` through a live judge via repo secrets, and emits a scored JSON artifact (`{per_metric, aggregate, verdict, baseline_ref}`). It **skips cleanly** (neutral, non-failing) when secrets are absent (the `golden_set --skip-unavailable` pattern).
- [ ] **AC-3** — A committed `docs/evaluation/humanness-baseline.json` defines per-metric expected scores + regression thresholds; both tiers read it, and "regression" means `current < baseline − threshold` for any gated metric.
- [ ] **AC-4** — Each tier uploads a named, retrievable CI artifact containing the scores + a boolean verdict; the deterministic tier's verdict is the merge-blocking status check, the scheduled tier's verdict is advisory + recorded.
- [ ] **AC-5** — A rollback runbook (`specs/measurement-loop/rollback.md` or `docs/operations/`) maps "measurement failed for gate X" → the exact launchd-plist env var to flip OFF + `launchctl bootout/bootstrap`, names the reinstall-regenerates-plist durability caveat, and proposes the durable fix (config-file or source default).
- [ ] **AC-6** — A reconciliation decision is recorded (in `design.md`) defining the policy that reconciles "6 gates already LIVE on a DISTINGUISHABLE proxy" with the gate-on-measurement contract: gates remain LIVE only while the automated proxy is ≥ baseline; a sustained proxy regression (or a filled human sheet reading DISTINGUISHABLE) triggers AC-5.
- [ ] **AC-7** — The scheduled job's artifact flags when the human blind-A/B sheet is **stale or blank** relative to the last proxy run, and the spec defines the trigger + owner + cadence for running the real human rating (the ground truth the proxy only approximates).

## Non-goals

- **Not** automating the *human* blind-A/B (humans can't be a CI step) — only tracking/escalating staleness of the human sheet.
- **Not** editing the daemon/agent activation-gate source (`src/daemon.c`, `src/agent/agent_turn.c`) — those are Seth's concurrently-edited lane; rollback operates via the launchd plist env, not source changes.
- **Not** building new eval scorers — reuse `shape.c`, persona-fidelity, turing, and `eval_blinded_ab.py`.
- **Not** making the secrets-tier (live-judge) job a merge-blocking required check — it is scheduled/advisory (flaky + secret-dependent, per `ci-required-checks` Tier policy). Only the deterministic tier blocks.
- **Not** retraining the model or changing what the gates *do* — this is measurement + gate-decision + rollback only.

## Constraints

- Live judge requires CI secrets (Vertex/Gemini OAuth + API key + PROJECT_ID); the scheduled job must no-op-skip (not fail) when they're unset.
- Mirror the proven `evaluation.yml` + `frontier-compare` + `baseline.json` + regression-threshold patterns; introduce no new eval infra.
- Deterministic tier must be secret-free and fast enough to be a PR-blocking Tier-1 check (`ci-required-checks.md`).
- Rollback is plist-env-based; the reinstall-regenerates-plist caveat must be documented with a durable-fix proposal.
- Built in an isolated worktree off `origin/main`; tasks that touch daemon/launchd activation or rollback are flagged **coordinate-with-Seth**, not blind-edit.
- The deterministic synthetic proxy must be reproducible (no `Date.now`/random nondeterminism in the gate) so a baseline comparison is meaningful.
