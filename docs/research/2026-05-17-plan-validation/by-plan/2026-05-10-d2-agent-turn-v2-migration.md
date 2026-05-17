---
plan: docs/plans/2026-05-10-d2-agent-turn-v2-migration.md
auditor: group-8-behavior-m3-master-sota
audited_at: 2026-05-17
implemented: NONE
proven: N/A
wired: N/A
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
This is an **intentionally deferred** decision doc, NOT an execution plan. It
captures why `agent_turn.c` continues to use a hybrid of v1 contact recall
(`hu_memory_recall_for_contact`) AND v2 world-model bridge
(`hu_w7_render_world_model`) instead of migrating fully to the W7 facade.
Migration is gated on four trigger criteria.

## Key Claims (from the plan)
- Decision: DEFER migration; document the hybrid in code comments
- Migration unblocked only when all four triggers met:
  W7 read benchmark, type collision resolved, eval suite stable, caller inventory
- All four triggers tracked as "Pending"

## Evidence

### Implemented? (code exists)
- The hybrid documented by the plan is intact. `agent_turn.c:1505` still calls
  `hu_memory_recall_for_contact` (v1 path), and `agent_turn.c:3527` calls
  `hu_w7_render_world_model` (v2 path). They compose as documented.
- No `use_w7_recall` agent flag has been added, no Phase 1 guard introduced.

### Proven? (tests exist)
- N/A — design/decision doc, no new code path expected.

### Wired? (called in runtime path / dispatch)
- N/A. Migration intentionally not executed.

## Gaps
- Plan calls for a "Memory recall: hybrid v1 + v2 by design" comment in
  `agent_turn.c` — I did not confirm the comment is present.
- Caller inventory of `hu_memory_recall_for_contact` (4th trigger) — current
  grep shows only 3 callers across src/. Plan claimed 14 in 2026-05-10;
  significant reduction has happened, but no formal inventory artifact exists.
- W7 read benchmark script `scripts/benchmark-w7-read.sh` — marked TBW; not
  created.
- W7 type collision plan (`docs/plans/2026-05-10-w7-type-collision-cleanup.md`)
  exists but no rename option has been chosen.

## Notes
- Verdict NOT_STARTED is the correct classification per the verdict tree
  (axes NONE/N/A/N/A) — this matches the plan's own DEFER decision.
- This is a healthy, honest deferral, not slipped work. SUPERSEDED would be
  wrong because the plan still represents the live decision.
