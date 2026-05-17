# Status Template — Plan Validation

Every per-plan status file MUST use this exact frontmatter so the master report
can mechanically aggregate.

```markdown
---
plan: <relative path from repo root, e.g. docs/plans/2026-05-10-w7-memory-facade.md>
auditor: <agent group name, e.g. group-7-memory>
audited_at: 2026-05-17
implemented: FULL | PARTIAL | NONE
proven: FULL | PARTIAL | NONE
wired: FULL | PARTIAL | NONE | N/A
verdict: SHIPPED | SHIPPED_UNWIRED | PARTIAL | NOT_STARTED | SUPERSEDED | OBSOLETE
confidence: HIGH | MEDIUM | LOW
---

## Plan Summary
<2-3 sentences: what the plan promised>

## Key Claims (from the plan)
- Claim 1: <what artifact/behavior the plan said would exist>
- Claim 2: ...

## Evidence

### Implemented? (code exists)
- <file:line citations or grep results>
- <or "NONE FOUND">

### Proven? (tests exist)
- <tests/test_*.c references or "NONE FOUND">

### Wired? (called in runtime path / dispatch)
- <call sites or "ORPHANED — no callers" or "N/A — design doc only">

## Gaps
<bulleted list of specific things missing>

## Notes
<superseded-by links, related plans, caveats>
```

## Verdict Decision Tree

- All three axes FULL → **SHIPPED**
- implemented=FULL, wired=NONE → **SHIPPED_UNWIRED** (orphan code)
- One or two axes PARTIAL → **PARTIAL**
- All three NONE → **NOT_STARTED**
- A newer plan explicitly replaces it → **SUPERSEDED**
- Plan is a design doc that was never meant to be executed (e.g., RFCs that were rejected) → **OBSOLETE**

## Confidence Rubric

- HIGH: cited specific file:line evidence, grepped for canonical symbols, found tests by name
- MEDIUM: grep hits but didn't read the code; or read the code but didn't verify tests
- LOW: only inferred from filenames or partial info
