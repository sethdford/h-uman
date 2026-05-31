---
title: CI SOTA optimization — faster, intelligent, self-healing
date: 2026-05-31
status: phase-1-shipped
---

# CI SOTA optimization

Grounded in observed pain from the 2026-05-31 reaction→adapter session, where
PR #207's full ~66-job matrix restarted **4 times** on review-fix commits
(~80 min runner-time, almost all obsolete the instant each commit landed), the
macOS jobs were consistently the long pole, and an LSan abort silently ate the
`Results:`/`FAIL` lines (`tail -3` on a fully-buffered pipe).

## Phase 1 — SHIPPED (this PR): safe, verified, biggest ROI

| Change | Files | Win |
|---|---|---|
| **Cancel superseded runs** (`concurrency: cancel-in-progress` on PR refs; never cancel `main`) | ci, m3-loop-smoke, codeql, benchmark | A new commit on a PR branch kills the in-flight matrix instantly — reclaims the ~15-20 min/commit churn |
| **Stop double-running** (scope `push:` to `branches: [main]`; PRs covered by `pull_request`) | ci, m3-loop-smoke | Every PR ran the whole matrix **twice** (push-any-branch + pull_request). Now once. |
| **Preserve failure output** (`stdbuf -oL` line-buffer + `tee` to `build-rl-sota/full-suite.log`) | validate-rl-sota.sh | LSan `abort_on_error` no longer discards the unflushed `Results:`/`FAIL` tail — failures are diagnosable again |
| **Upload the full-suite log** as a CI artifact | rl-nightly | The preserved log is retrievable for 30 days |

Phase 1 alone roughly **halves** runner load on PRs (no double-run) and removes
the churn-rebuild waste (cancel-in-progress). All edits `actionlint`-clean.

## Phase 2 — affected-only CI (needs a throwaway-branch CI dry-run)

The repo already ships `scripts/what-to-test.sh` (changed-files → suites). Wire
it in WITHOUT deadlocking required checks:

- Add a `changes` job using `dorny/paths-filter` exposing outputs (`code`,
  `ui`, `web`, `docs`, `android`, `tokens`).
- Gate each non-universal job with `if: needs.changes.outputs.<area> == 'true'`.
  A job skipped via job-level `if:` reports **skipped**, which **satisfies**
  required status checks (no merge deadlock) — verify on a scratch PR first.
- For `build-and-test`, keep the full compile but run only affected suites on
  PRs via `what-to-test.sh`; run the FULL suite on `main` + nightly (never
  reduce coverage on the protected branch).

Why dry-run-gated: mis-setting `needs`/`if` across ~30 jobs can either
over-skip (let regressions through) or deadlock branch protection. This must be
validated against a real PR's check accounting, not edited blind.

## Phase 3 — fail-fast staging

- **Stage 0 (~2 min):** compile all presets + `clang-format`/lint +
  changed-suite tests. 90% of breakage dies here instead of 20 min in.
- **Stage 1:** full matrix, `needs: [stage0]`.
- **Stage 2 (label/nightly):** rl_sota+LSan, fuzz, benchmarks.
- Split the slow Linux **LSan** gate: fast **ASan-only** on PRs + full **LSan**
  nightly, so PRs aren't blocked on the heaviest job.

## Phase 4 — caching depth

- `ccache`/`sccache` via `CMAKE_C_COMPILER_LAUNCHER` in `.github/actions/setup-build`
  (keyed on compiler+flags) — complements the existing whole-`build`-dir cache,
  which fully misses on any `src/**` change.
- Cache the **llama.cpp / mlx** build trees for the rl_sota nightly (its long
  pole is a from-scratch llama.cpp build every run).

## Phase 5 — intelligent / self-healing (wire existing assets)

| Capability | Mechanism | Existing asset |
|---|---|---|
| Systemic-vs-per-PR triage | cluster failures by signature; O(root-causes) not O(PRs) | `/diagnose-ci-queue`, `ci-queue-triage.md` |
| Auto-bisect red `main` → tracked issue (+ optional auto-revert past N min) | dispatch on `main` failure | `regression-hunter` agent |
| Flaky-test quarantine + auto-retry, tracked | detect non-determinism | `flake-detector` agent |
| Auto-retry transient infra (network/runner) | `nick-fields/retry` on flaky steps; gate only `required` (advisory `ui-e2e`/`visual-regression` never block) | `ci-required-checks.md` tiers |
| Predictive test selection | ML-rank tests by failure probability | net-new |
| Generated stats are generated, not hand-edited | a CI check/commit regenerates binary-size/test-count lines in README/CLAUDE.md/AGENTS.md | (kills the `2775` vs `23209 KB` drift) |

## Sequencing

Phase 1 shipped here. Phases 2-3 are the next-biggest wins and should land
together after a scratch-PR dry-run of the skip-as-green accounting. Phase 4 is
independent and low-risk. Phase 5 items are separate features, each its own PR.
