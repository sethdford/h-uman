---
plan: docs/plans/2026-03-10-human-fidelity-phase3-superhuman-memory.md
auditor: group-4-human-fidelity
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Phase 3: BTH (Better Than Human) memory — micro-moments, inside jokes,
commitments with deadlines, avoidance/pattern detection, temporal learning,
spontaneous curiosity, callbacks, calendar awareness, birthday/holiday
messages. New module `src/memory/superhuman.c` + extensive SQLite schema.
Features F18-F24, F26, F30-F31, F50, F53.

## Key Claims (from the plan)
- Claim 1: `src/memory/superhuman.c` + `include/human/memory/superhuman.h`
- Claim 2: New SQLite tables: `inside_jokes`, `commitments`,
  `temporal_patterns`, `delayed_followups`, `avoidance_patterns`,
  `topic_baselines`, `micro_moments`, `growth_milestones`
- Claim 3: Daemon proactively calls superhuman queries
- Claim 4: Agent injects micro-moment / inside-joke / pattern / callback
  context per turn

## Evidence

### Implemented? (code exists)
- `src/memory/superhuman.c` — present
- `include/human/memory/superhuman.h` — present
- Adjacent specialty modules: `include/human/agent/superhuman.h`,
  `superhuman_commitment.h`, `superhuman_emotional.h`, `superhuman_predictive.h`,
  `superhuman_silence.h`
- Implementation modules: `src/agent/superhuman.c`, `superhuman_silence.c`,
  `superhuman_emotional.c`, `superhuman_predictive.c`, `superhuman_commitment.c`
- SQLite schema_parts includes delayed_followups (verified via
  `src/memory/superhuman.c:462,476,490,505` which read/write
  `delayed_followups`)

### Proven? (tests exist)
- `tests/test_superhuman.c`
- Adjacent: `tests/test_persona_delta_observer.c`,
  `tests/test_personal_model*.c` (memory of user)

### Wired? (called in runtime path / dispatch)
- `src/daemon.c:639` — `hu_superhuman_topic_baseline_record(...)`
- `src/daemon.c:1320` — `hu_superhuman_temporal_get_quiet_hours(...)`
- `src/daemon.c:1420-1450` — `hu_superhuman_commitment_list_due(...)` +
  `hu_superhuman_commitment_free(...)` (proactive cycle, commitment
  follow-ups)
- `src/agent/agent.c`, `agent_turn.c`, `proactive.c` all include and call
  `hu_superhuman_*` (verified via grep — at least 8 caller files)

## Gaps
- None material at the integration level. Per-feature depth (e.g. growth
  milestones vs micro-moments) was not exhaustively verified but the
  scaffolding is fully present and active.

## Notes
- One of the largest deltas in the project; lots of files added, all wired.
