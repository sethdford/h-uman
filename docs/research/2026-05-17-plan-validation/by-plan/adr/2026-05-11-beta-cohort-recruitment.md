---
plan: docs/plans/adr/2026-05-11-beta-cohort-recruitment.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: NONE
verdict: NOT_STARTED
confidence: MEDIUM
---

## Plan Summary
ADR defining a two-phase beta recruitment posture: friends-and-family invite-only at
Months 4-5 (target 20-30 users), gated public waitlist at Month 6 (C6.1), with explicit
consent forms, no PII in repo, telemetry stays local, `human beta opt-out` CLI subcommand,
and legal review before public phase.

## Key Claims (from the plan)
- Claim 1: `docs/legal/beta-consent.md` consent form
- Claim 2: `human beta opt-out` one-command operation surviving daemon restart
- Claim 3: Landing-page privacy disclosure
- Claim 4: Linear project tracking, no PII in issue text

## Evidence

### Implemented? (code exists)
- `ls docs/legal/` — directory does NOT exist
- `grep -rn "human beta\|HU_BETA" src/main.c` returns 0 hits
- No CLI subcommand for beta opt-out

### Proven? (tests exist)
- NONE FOUND

### Wired? (called in runtime path / dispatch)
- N/A — nothing implemented

## Gaps
- All claims unimplemented (consistent with ADR's Month-4/5 schedule)

## Notes
ADR is "Accepted" and scheduled for Months 4-5 / Month 6 — well after today's
2026-05-17 date. NOT_STARTED is the expected state. The decision itself is intact;
nothing in the codebase contradicts it.
