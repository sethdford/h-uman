---
plan: docs/plans/2026-05-16-audit-followups/README.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Index doc for the May 16 audit followups. Calls out the two highest-impact fixes that
LANDED in the audit session (`HU_PERM_DENY` for unknown-tool masquerading; sandbox
deny-by-default in `src/tools/shell.c`) and lists 5 follow-up specs/plans (01-05).

## Key Claims (from the plan)
- Claim 1: `HU_PERM_DENY` sentinel landed; 4 adversarial tests rewritten
- Claim 2: `hu_shell_must_deny_unsandboxed` extracted; sandbox no longer falls through to bare `/bin/sh`
- Claim 3: 5 audit follow-up plans are queued for execution in priority order

## Evidence

### Implemented? (code exists)
- LANDED fixes confirmed:
  - `src/tools/shell.c:57` — `hu_shell_must_deny_unsandboxed()` predicate exists
  - `src/tools/shell.c:250` — predicate is called in fork path
  - `tests/test_shell_sandbox.c` — 7 truth-table tests covering all combinations
- HU_PERM_DENY: not directly verified in this audit pass (covered by adjacent group)
- Five follow-up plans (01-05) all on disk

### Proven? (tests exist)
- Shell sandbox: `tests/test_shell_sandbox.c` confirmed
- Two highest-impact fixes carry their test coverage

### Wired? (called in runtime path / dispatch)
- Sandbox predicate called from real fork path (line 250)

## Gaps
- Per the audit-followup child docs (01-05): only 04 (daemon decomposition) shows
  partial progress; 01, 02, 03, 05 are all NOT_STARTED.
- The "two highest-impact fixes" claim holds; everything else queued is still queued.

## Notes
The README itself is accurate as a status document. The forward-looking promises (the
five queued plans) are mostly unfulfilled. See individual child status files for
breakdowns.
