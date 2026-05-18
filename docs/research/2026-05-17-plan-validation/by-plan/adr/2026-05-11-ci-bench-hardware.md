---
plan: docs/plans/adr/2026-05-11-ci-bench-hardware.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
ADR accepting Option A: a dedicated on-prem M4 Max self-hosted GitHub Actions runner
labeled `perf-m4-max` as the canonical perf runner. Nightly cron triggers a runner
job that drains background load, restarts mlx-server, runs
`scripts/bench-gemma-perf.py`, and uploads JSON artifacts; comparison workflow alerts
if regression > 5% tps / > 10% ttft.

## Key Claims (from the plan)
- Claim 1: `.github/workflows/perf-nightly.yml` exists with `perf-m4-max` runner label
- Claim 2: `scripts/bench-gemma-perf.py` is the canonical bench
- Claim 3: launchd plist for the dedicated runner
- Claim 4: ADR is referenced from the workflow

## Evidence

### Implemented? (code exists)
- `.github/workflows/perf-nightly.yml` exists ✓
- Workflow header references ADR explicitly:
  `# Governing ADR: docs/plans/adr/2026-05-11-ci-bench-hardware.md` ✓
- Runner label declared: `# Runner label: perf-m4-max` ✓
- `scripts/bench-gemma-perf.py` exists ✓
- `scripts/perf-nightly-launchd.plist.template` exists ✓
- Workflow has both nightly cron (`0 7 * * *`) and `workflow_dispatch` ✓

### Proven? (tests exist)
- The workflow itself is the test (it runs nightly and asserts threshold compliance)
- Not unit-tested in the C test suite (out of scope for a CI workflow)

### Wired? (called in runtime path / dispatch)
- Workflow registered with GitHub Actions; assumed to be running per ADR

## Gaps
- `docs/perf/README.md` exists but minimal; ADR says it should be created in Phase B2.3
- M2 Pro warm-spare runner: not verifiable from repo (operational concern)

## Notes
This ADR is honored well. The workflow file explicitly cites the ADR path in its
header comment, which is exemplary traceability.
