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

## Phase 2 — affected-only CI (frontend/docs jobs) — SHIPPED

A `changes` job (`dorny/paths-filter`) sets `ui`/`web`/`docs`/`tokens` booleans;
the frontend/docs jobs (`ui`, `ui-e2e`, `website`, `design-tokens`, `docs`,
`visual-regression`) gate on `if: <area> || github.event_name != 'pull_request'`.
A pure-backend PR skips all of them; main/nightly always run full. Job-level
`if:` skips report "skipped" → **satisfy required checks** (no deadlock), so no
branch-protection change is needed — verified on this PR's own run. The C/build
matrix still always runs (a docs-only PR skipping the C suite is a future,
higher-risk extension — see below).

### Original notes (kept for the backend-skip extension)

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

- ~~`ccache` via `CMAKE_C_COMPILER_LAUNCHER`~~ **ALREADY PRESENT** in
  `.github/actions/setup-build` (installs ccache, caches `~/.cache/ccache`
  content-addressed keyed on src hash + prefix restore-keys, sets the launcher).
  Verified 2026-05-31 — no work needed; the original note here was wrong.
- ~~Remaining: cache the **llama.cpp** build for the rl_sota nightly~~ **DONE.**
  The nightly's `validate-rl-sota.sh` ran its own `cmake --preset rl_sota` with
  NO compiler launcher (setup-build's launcher applies to the `build` dir, not
  the separate `build-rl-sota`), so llama.cpp recompiled from scratch every run
  — the nightly's long pole. Fixed: validate-rl-sota.sh now adds
  `CMAKE_C/CXX_COMPILER_LAUNCHER=ccache` (guarded on ccache present), and
  rl-nightly.yml bumps `ccache max_size=2G` so the vendored llama.cpp objects
  (ggml + llama) fit alongside h-uman's and persist across runs. ccache is
  content-addressed → no stale-object risk; no-op locally without ccache.

## Phase 5 — intelligent / self-healing (wire existing assets)

| Capability | Mechanism | Existing asset | Status |
|---|---|---|---|
| Auto-retry transient infra (network/runner) | bounded bash `until` retry (×3, exp backoff, fail-loud) around `apt-get update`/`apt-get install`/`brew install` in `setup-build` | zero-dep, no third-party action | **SHIPPED** |
| Auto-triage red `main` → tracked issue | `ci-autotriage.yml` (`workflow_run` on Human CI / M3 smoke / RL Nightly) opens/updates ONE deduped issue: failed-run URL, commit range since last-green, ready-to-run bisect harness | `regression-hunter` agent, `ci-queue-triage.md` | **SHIPPED** |
| Systemic-vs-per-PR triage | cluster failures by signature; O(root-causes) not O(PRs) | `/diagnose-ci-queue`, `ci-queue-triage.md` | future PR |
| Flaky-test quarantine + auto-retry, tracked | `HU_RUN_TEST_FLAKY` in `test_framework.h`: retries a known-flaky test, recovers on pass, still FAILS if all attempts fail; call sites = the quarantine registry; `HU_TEST_FLAKY_RETRIES` tunable | `flake-detector` agent | **SHIPPED** |
| Predictive test selection | `predict-tests.sh`: header-level reverse-`#include` reachability → the suites a changed header actually reaches (precise where `what-to-test.sh`'s directory map is coarse); caps to "full suite" for hot/central headers | net-new | **SHIPPED** (deterministic; ML failure-probability ranking still needs a failure-history corpus — future) |
| Generated stats are generated, not hand-edited | extend `check-metrics-drift.sh`: lines-of-C gated build-free (doc-fleet + pre-push); new `--binary` mode gates the `~NNNNN KB` claims in the `release-size` job. Fixed the live drift: binary `~23209 KB`→`~2468 KB` (9.4x), LOC `~476K`→`~430K`. | `repo-metrics.sh`, `update-stats.sh` | **SHIPPED** |

### Phase 5 shipped here — design notes

**Auto-retry transient infra** (`.github/actions/setup-build/action.yml`). apt
mirror 5xx / DNS blips / dpkg-lock contention on shared runners are a recurring
red cause unrelated to the change under test. A `retry()` bash helper wraps the
three network-bound install commands: up to 3 attempts, `n*5s` (apt) / `n*10s`
(brew) backoff, and an `exit 1` (fail-loud) after the third so a genuinely
broken install still goes red. `until <cmd>` is exempt from `set -eo pipefail`,
so the final failure propagates via the explicit `exit`. Zero new dependency
(no `nick-fields/retry` SHA to pin/maintain — keeps the action self-contained).

**Auto-triage red `main`** (`.github/workflows/ci-autotriage.yml`). A
`workflow_run`-triggered job (fires only on `conclusion == 'failure'` +
`head_branch == 'main'`) files a single, deduped `ci-red-main`-labelled tracking
issue per workflow with: the failed run URL, the commit range since that
workflow's last green run on main (via `gh run list --status success`), the
suspect-commit list, and a paste-ready `git bisect` harness pointing at
`regression-hunter`. Subsequent failures append a comment rather than spawn
duplicates (exact-title match in awk). Security: the only untrusted input (the
head-commit subject) is passed solely as a `printf` `%s` argument and to gh/jq
via env/`-v`, never interpolated into a shell command or an expanding heredoc —
verified locally against an adversarial `$(touch …)` + backtick subject (nothing
executed). `actionlint`-clean.

## Sequencing

Phase 1 shipped here. Phases 2-3 are the next-biggest wins and should land
together after a scratch-PR dry-run of the skip-as-green accounting. Phase 4 is
independent and low-risk. Phase 5 items are separate features, each its own PR.
