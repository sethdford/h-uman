---
plan: docs/plans/adr/2026-05-11-public-benchmark-license.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: NONE
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
ADR accepting a three-license split for public benchmark publication:
CC-BY-4.0 for scenarios + judge prompts, CC-BY-SA-4.0 for methodology paper,
Apache-2.0 for reference implementation, CDLA-Permissive-2.0 for bench result
artifacts. Repo LICENSE file lands before A6.3; CITATION.cff at root.

## Key Claims (from the plan)
- Claim 1: Repo-root `LICENSE` file uses Apache-2.0
- Claim 2: `CITATION.cff` at repo root
- Claim 3: `eval_suites/LICENSE.md` with per-artifact license breakdown
- Claim 4: License headers in `scripts/bench-gemma-perf.py` and harness code
- Claim 5: `docs/perf/competitive/methodology.md` carries CC-BY-SA-4.0
- Claim 6: Privacy audit completed before any scenario publication

## Evidence

### Implemented? (code exists)
- `LICENSE` at repo root EXISTS but is **MIT License** (Copyright 2026 human contributors),
  NOT Apache-2.0 as the ADR specifies
- `CITATION.cff` does NOT exist at repo root
- `eval_suites/LICENSE.md` does NOT exist
- `docs/perf/competitive/methodology.md` does NOT exist (only `docs/perf/README.md` and
  `baseline.json` exist)
- `scripts/bench-gemma-perf.py` exists but not checked for the planned license headers

### Proven? (tests exist)
- N/A — licensing is policy, not testable

### Wired? (called in runtime path / dispatch)
- N/A

## Gaps
- The current LICENSE file CONTRADICTS the ADR (MIT vs Apache-2.0). Either the ADR
  should be amended to MIT or the LICENSE should be replaced.
- All other artifacts (CITATION.cff, eval_suites/LICENSE.md, methodology.md) absent.
- ADR scheduled for Phase E2 + month 5 legal review — both are future, so absence is
  schedule-consistent except for the LICENSE-file divergence.

## Notes
The ADR's Status section says "Bookkeeping work (LICENSE files, headers, CITATION.cff)
lands in Phase E2". The current repo-root LICENSE = MIT is a deviation; whoever
adopted MIT did so without amending this ADR. Flag this for product/legal review.
