# Automated Humanness Measurement Loop — Tasks

> Phase 3. Sequenced: **non-colliding measurement-automation first (buildable now,
> all new files)**, then **rollback/durable-gate (coordinate-with-Seth)**. The
> live-judge tier's CI-secrets dependency is called out explicitly.

| # | Task | ACs | Owner | Status |
|---|---|---|---|---|
| 1 | Add `docs/evaluation/humanness-baseline.json` — per-metric `{expected, threshold}` for the deterministic scorers (shape/fidelity/turing) and the blind-A/B proxy; document each metric's source. Seed `expected` from a first local run, not invented numbers. | AC-3 | me / general-purpose | pending |
| 2 | Add `scripts/check_humanness_regression.py` (or extend `scripts/eval_check_regression.py`) — pure: `(scores.json, humanness-baseline.json) → {pass, regressed_metrics[]}`, regression = `current < expected − threshold`. Unit-tested on fixtures (pass / single-metric-regress / missing-metric). | AC-1, AC-2, AC-3 | me / general-purpose | pending |
| 3 | Add a **deterministic** synthetic-proxy runner — runs `shape` + persona-fidelity + turing scorers over a committed synthetic prompt set, fixed-seed/no-wall-clock, emits `humanness-scores.json`. Prefer a `human evaluation` subcommand or a thin python harness over the existing scorers; assert reproducibility (same input → byte-identical scores). | AC-1, AC-6 (D6) | me / general-purpose | pending |
| 4 | Add `.github/workflows/humanness.yml` job `humanness-deterministic` — PR trigger, secret-free: build `human`, run task-3 runner, run task-2 check, upload `humanness-scores.json` artifact, expose an **advisory** status (D2). | AC-1, AC-4 | me / general-purpose | pending |
| 5 | Add job `humanness-blind-ab` to the same workflow — `schedule` (cron, off-peak minute) + `workflow_dispatch`, secrets-gated: run `scripts/eval_blinded_ab.py --synthetic` via the live judge, run task-2 check vs baseline, upload `blind-ab-scores.json`. **Skip neutral** (exit 0) when secrets unset (`golden_set --skip-unavailable` pattern). **DEP: CI secrets** (Vertex/Gemini — reuse `frontier-compare`'s secret names). | AC-2, AC-4 | me / general-purpose | pending |
| 6 | Add the human-sheet staleness check — the scheduled job inspects the blind-A/B rating sheet (present? empty? mtime vs this run) and writes `human_sheet:{present,stale_vs_run}` into `blind-ab-scores.json`; add an escalation note (trigger + owner + cadence) to run the real human rating. | AC-7 | me / general-purpose | pending |
| 7 | Write `specs/measurement-loop/rollback.md` — failing-measurement → table mapping each gate to its plist env var (`HU_GRAPH_GROUNDING`, `HU_SALIENCE_LIVE`/`_SHADOW`, `HU_BANDIT_HUMANIZATION`, ToM/self-uncertainty/intent vars) → flip OFF/SHADOW + `launchctl bootout`/`bootstrap`; document the reinstall-regenerates-plist caveat + name the durable fix; include the stricter contract-honest option (roll all 6 to SHADOW now). | AC-5, AC-6 | me (verify var names w/ Seth) | pending |
| 8 | Record the reconciliation policy (D5) as an operator-facing note linked from `rollback.md`: gates stay LIVE while proxy ≥ baseline; sustained proxy regression OR a filled human sheet reading DISTINGUISHABLE → run task-7 runbook (operator-initiated, not auto). | AC-6 | me | pending |
| 9 | **[COORDINATE-WITH-SETH]** Durable gate fix — move the 6 gate defaults off plist-env-only onto a config-file/source default so rollback survives a reinstall. Touches `src/daemon.c`/config/launchd (Seth's active lane) → do NOT blind-edit; pair with Seth. | AC-5 (durability) | Seth + me | pending |
| 10 | **[PROMOTE, LATER]** After N clean `humanness-deterministic` runs prove the synthetic proxy is reproducible + non-flaky, flip it from advisory → required status check (branch protection, `ci-required-checks.md`). | AC-1 (D2) | me | pending |

## Coverage check (every AC ↔ ≥1 task)

- AC-1 → 3, 4, 10  · AC-2 → 2, 5  · AC-3 → 1, 2  · AC-4 → 4, 5  · AC-5 → 7, 9  · AC-6 → 7, 8 (+ D5 in design)  · AC-7 → 5, 6

## Dependencies

- 2 → 1 ; 4 → 1,2,3 ; 5 → 1,2 (+ **CI secrets**) ; 6 → 5 ; 8 → 7 ; 9 → 7 ; 10 → 4 + track record.
- **Buildable now (non-colliding, no secrets): 1, 2, 3, 4, 7, 8.** Task 5/6 need CI secrets. Task 9 needs Seth. Task 10 needs a track record.

## Dispatch notes

- Tasks 1–4, 7, 8 are independent of Seth's daemon edits → safe in this isolated worktree now. They could be one `general-purpose` agent or done sequentially.
- Task 5/6 are gated on you provisioning the Vertex/Gemini CI secrets (same names as `frontier-compare`). Until then the job exists but skips neutral.
- Task 9 (durable gate) is the only one that touches Seth's lane — schedule it with him, never blind-edit.
- The synthetic proxy currently reads DISTINGUISHABLE: do NOT promote task 10 to blocking, and do NOT auto-rollback on the proxy, until it's correlated against ≥1 real human-sheet run (design Risk #1).
