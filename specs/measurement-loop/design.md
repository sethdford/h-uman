# Automated Humanness Measurement Loop — Design

> Phase 2. Built on the approved `requirements.md`. Reuses the proven
> `evaluation.yml` / `frontier-compare` / `baseline.json` / regression-threshold
> infra — introduces no new eval framework.

## Components

- **Deterministic humanness gate** (`.github/workflows/humanness.yml`, job `humanness-deterministic`) — secret-free, runs on every PR. Builds `human`, runs the existing `shape` + persona-fidelity + turing scorers over a committed synthetic prompt set + a reproducible deterministic proxy, aggregates, compares to baseline. Emits `humanness-scores.json` artifact + a status check. — serves AC-1, AC-3, AC-4.
- **Live-judge proxy job** (same workflow file, job `humanness-blind-ab`, `schedule` cron + `workflow_dispatch`) — secrets-gated. Runs `scripts/eval_blinded_ab.py --synthetic` through the live judge (Vertex/Gemini), scores vs baseline, uploads `blind-ab-scores.json`. Skips neutral (exit 0, "skipped: no secrets") when secrets unset. — serves AC-2, AC-3, AC-4, AC-7.
- **Humanness baseline** (`docs/evaluation/humanness-baseline.json`) — sibling to the W16 `baseline.json`; per-metric `{expected, threshold}` for both tiers. — serves AC-3, AC-6.
- **Threshold check** (`scripts/check_humanness_regression.py` or reuse `scripts/eval_check_regression.py`) — pure: takes a scores file + the baseline, returns pass/fail + which metrics regressed. Used by both jobs so the regression semantics are identical. — serves AC-1, AC-2, AC-3.
- **Rollback runbook** (`specs/measurement-loop/rollback.md`) — failing-measurement → plist-env flip table + `launchctl bootout/bootstrap` + durability caveat/fix. — serves AC-5.
- **Reconciliation policy** (this doc, Decisions D5) + **human-sheet staleness flag** (a field in `blind-ab-scores.json` + a step in the scheduled job that inspects the human sheet's mtime/emptiness). — serves AC-6, AC-7.

## Data flow

1. **PR opened** → `humanness-deterministic` builds `human`, runs scorers + deterministic synthetic proxy → `humanness-scores.json` → `check_humanness_regression.py` vs `humanness-baseline.json` → verdict. Advisory now (D2); becomes the blocking status check after the track-record promotion.
2. **Nightly cron / dispatch** → `humanness-blind-ab`: if secrets present, `eval_blinded_ab.py --synthetic` → judge scores → threshold check → `blind-ab-scores.json` (incl. `human_sheet: {present, stale_vs_run}`). If absent → neutral skip.
3. **Verdict = fail** (sustained, per D5) → operator opens `rollback.md` → flips the offending gate's plist env var OFF → `bootout`/`bootstrap` → gate returns to SHADOW/OFF; the change is recorded.
4. **Human sheet stale/blank** → scheduled artifact flags it → escalation per AC-7 (owner + cadence) → once filled, the human verdict supersedes the proxy (D5).

## Decisions

- **D1 — Two tiers, only the deterministic one blocks.** Mirrors `evaluation.yml` (auto, gating) + `frontier-compare` (dispatch, secrets, advisory). The live judge is nondeterministic, costs API calls, and needs secrets → unfit as a required PR check (`ci-required-checks.md` Tier policy). Serves AC-1, AC-2 + the "not a blocking check" non-goal.
- **D2 — Deterministic gate ships ADVISORY, promotes to blocking after a clean track record.** A brand-new synthetic proxy whose reproducibility isn't yet proven must not block merges on day one (`ci-required-checks` promote-after-track-record). Reversible: flip to `required` once stable. Serves AC-1. *(Open call you deferred — chosen conservative.)*
- **D3 — New `humanness-baseline.json`, same shape/semantics as W16 `baseline.json`.** Regression = `current < expected − threshold` per gated metric; baseline updates land via a PR (like evaluation.yml's baseline-update-PR step), so changes are reviewed. Serves AC-3.
- **D4 — Rollback operates on the launchd plist env, never the daemon/agent source.** `src/daemon.c` / `src/agent/agent_turn.c` are Seth's concurrently-edited lane; the plist env (`HU_GRAPH_GROUNDING`, `HU_SALIENCE_LIVE`/`_SHADOW`, `HU_BANDIT_HUMANIZATION`, + the ToM/self-uncertainty/intent vars) is the live control surface. Serves AC-5 + the "don't edit activation source" non-goal.
- **D5 — Gates stay LIVE pending the human sheet; proxy regression is the automated tripwire, not an auto-rollback.** Honors the current daemon state (Seth flipped all 6 live). The human blind-A/B sheet remains ground truth; the proxy only approximates it. Policy: stay LIVE while proxy ≥ baseline; a *sustained* proxy regression OR a filled human sheet reading DISTINGUISHABLE triggers the AC-5 runbook (operator-initiated, not auto). Contract-honest alternative (roll all 6 to SHADOW now, since the proxy reads DISTINGUISHABLE) is documented in `rollback.md` as the stricter option. Serves AC-6. *(Open call you deferred — chosen to honor your flip while keeping the contract path explicit.)*
- **D6 — The synthetic proxy must be deterministic (seedless / fixed-seed, no wall-clock).** A baseline comparison is meaningless if the gate's own score wanders run-to-run. Serves AC-1, AC-3.
- **D7 — The human sheet's staleness is a first-class field in the scheduled artifact.** Prevents the proxy from silently becoming unchallenged truth. Serves AC-7.

## Risks

- **Proxy ≠ human judgment** (it reads DISTINGUISHABLE — may be miscalibrated). Mitigation: advisory-first (D2), no auto-rollback (D5), human-sheet escalation (D7). Do not let the proxy alone flip production behavior until it's correlated against ≥1 real human-sheet run.
- **Live-judge nondeterminism/cost.** Mitigation: scheduled-not-PR + skip-clean on no-secrets (AC-2).
- **Plist rollback isn't durable** (reinstall regenerates the plist, re-dropping the gates). Mitigation: document in `rollback.md` + propose the durable fix (config-file gate or source default) as a coordinate-with-Seth follow-up task.
- **Baseline gaming / staleness.** Mitigation: baseline changes via reviewed PR (D3); the scheduled job records drift over time.
- **Collision with Seth's activation edits.** Mitigation: measurement-run components are all new files (non-colliding); the rollback + any durable-gate fix are flagged coordinate-with-Seth in `tasks.md`.
- **Secrets absent in forks/CI.** Mitigation: the live-judge job skips neutral, never fails the run (golden-set `--skip-unavailable` pattern).
