---
plan: docs/plans/2026-05-16-audit-followups/04-daemon-decomposition.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Decompose 12,262-LOC `src/daemon.c` into 7 cohesive sibling modules
(`src/daemon/{daemon.c,routing.c,director.c,telemetry.c,lifecycle.c,ticks.c,inbox.c,outbound.c}`),
each ≤ 1,500 LOC with dedicated tests, in 7 staged PRs. No behavior change.

## Key Claims (from the plan)
- Claim 1: 7-module decomposition under `src/daemon/`
- Claim 2: daemon.c shrinks to < 800 LOC top-level main + event loop
- Claim 3: Each split module has a dedicated `tests/test_daemon_*.c` file
- Claim 4: `scripts/gen-include-graph.sh` verifies the dependency DAG
- Claim 5: Module dependency graph is acyclic, reviewable end-to-end

## Evidence

### Implemented? (code exists)
- `wc -l src/daemon.c` → **12,491 LOC** (slightly LARGER than audit's 12,262 baseline)
- `ls src/daemon/` → directory does NOT exist
- BUT: SIBLING extractions exist (not in `src/daemon/`):
  - `src/daemon_cron.c` (413 LOC)
  - `src/daemon_lifecycle.c` (510 LOC)
  - `src/daemon_proactive.c` (723 LOC)
  - `src/daemon_routing.c` (104 LOC)
  - Total extracted: 1,750 LOC
- No `director.c`, `telemetry.c`, `ticks.c`, `inbox.c`, `outbound.c` modules

### Proven? (tests exist)
- Dedicated test files exist for the four extracted siblings:
  - `tests/test_daemon_cron.c`
  - `tests/test_daemon_lifecycle.c`
  - `tests/test_daemon_proactive.c`
  - `tests/test_daemon_routing.c`
  - Plus `tests/test_daemon_e2e_validator.c`, `test_daemon_housekeeping.c`, `test_daemon_trust.c`
- No `tests/test_daemon_baseline.c` (the Phase 0 baseline gate)

### Wired? (called in runtime path / dispatch)
- Extracted modules ARE called from daemon.c (they're production code, not orphans)

## Gaps
- daemon.c is still ~10,700 LOC (12,491 - 1,750 extracted) — far above Phase 7's
  < 800 LOC target
- 5 of the 7 planned modules not extracted
- No `src/daemon/` directory; extractions use flat `daemon_*.c` filenames instead
- Phase 0 baseline test (`test_daemon_baseline.c`) absent — the gate that's supposed
  to prevent behavior drift between phases
- `scripts/gen-include-graph.sh` DAG check not gated in CI for these modules

## Notes
The plan was always staged ("3-4 weeks, do NOT attempt as a single PR"). Real progress
has been made — 4 sub-modules + their tests — but the work is incomplete and daemon.c
itself has grown rather than shrunk. The decomposition pattern (flat siblings vs.
`src/daemon/` folder) diverges from the plan's proposal; this is a benign deviation
but means Phase 7's "daemon/CLAUDE.md" mini-doc target needs adjustment.
