---
plan: docs/plans/adr/2026-05-11-adapter-rollback-signal.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: NONE
verdict: NOT_STARTED
confidence: MEDIUM
---

## Plan Summary
ADR defining the multi-signal AND auto-rollback policy for nightly-promoted LoRA
adapters: persona-eval regression (S1), perplexity drift (S2), user-feedback
signal (S3), any of which trips auto-rollback to last-known-good adapter with
7-day promotion lock and manual `--force` reinstate.

## Key Claims (from the plan)
- Claim 1: Three signals S1/S2/S3 defined with explicit thresholds
- Claim 2: Auto-rollback procedure (detect → switch → lock → incident record → notify)
- Claim 3: `human ml adapter promote --force <adapter-id>` for manual reinstate
- Claim 4: `<adapter>.metadata.json` stores anchor scores alongside the adapter
- Claim 5: Last-known-good retained ≥ 30 days or 3 rollback cycles
- Claim 6: Documented in `src/ml/CLAUDE.md` and `docs/standards/ai/evaluation.md`

## Evidence

### Implemented? (code exists)
- `grep -rn "rollback\|auto.rollback\|adapter.*rollback\|HU_ADAPTER_ROLLBACK" src/ml/`
  returns 0 hits
- `src/ml/` contains agent_trainer.c, checkpoint.c, cli.c, etc. — no rollback module
- `human ml adapter promote --force` CLI flag not present
- No `<adapter>.metadata.json` schema

### Proven? (tests exist)
- No tests for any of the three rollback signals
- No test exercising the auto-rollback path

### Wired? (called in runtime path / dispatch)
- N/A — no implementation to wire

## Gaps
- All three rollback signals (S1/S2/S3) unimplemented
- Auto-rollback procedure unimplemented
- `--force` reinstate CLI not added
- Adapter metadata file format not defined
- `src/ml/CLAUDE.md` does not have a rollback section
- `docs/standards/ai/evaluation.md` does not have a rollback policy section

## Notes
The ADR is "Accepted" and scheduled per the SOTA 6-month roadmap (Phase A5). Today's
date is 2026-05-17, well before Month-5 A5 milestone. Treating as NOT_STARTED is
correct relative to the schedule; the decision itself is intact.
