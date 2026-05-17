---
plan: docs/plans/2026-05-11-init-10-episode-storage-sleep-consolidation.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
First-class **episodes** (verbatim user-turn + assistant-turn + tool-calls
+ verifier scores + outcome) become the ground truth in memory.
Consolidation becomes a two-phase scheduler-driven process: **NREM**
compresses + indexes episode batches; **REM** synthesizes higher-order
beliefs into `hu_personal_model_t`. Read path queries episodes first.

## Key Claims (from the plan)
- Episode struct gains verbatim text, verifier scores, tool-call trace
- NREM scheduler phase (lossy-but-recoverable compression)
- REM scheduler phase (belief synthesis into personal model)
- Read path queries episodes first, summaries fall back
- New entry points in `include/human/memory/episodic.h` and `include/human/agent/scheduler.h`

## Evidence

### Implemented? (code exists)
- Episode storage subsystem **predates the plan** and exists in three places (the plan acknowledges this):
  - `src/memory/episodic.c`, `include/human/memory/episodic.h`
  - `src/agent/episodic.c`, `include/human/agent/episodic.h`
  - `src/memory/deep_memory.c`, `include/human/memory/deep_memory.h`
- Consolidation engine exists: `src/memory/consolidation.c`, `src/memory/consolidation_engine.c`.
- Grep for `SleepGate`, `sleep_gate`, `NREM`, `REM_consolidation`, `hu_sleep_gate`, `sleep_consolidation` — **zero hits outside the plan doc**. The plan's two-phase NREM/REM terminology has not been adopted in code.
- The verbatim-turn-text + verifier-score augmentation to `hu_episode_t` is not visible at the include-graph level (`hu_episode_t` still appears to be the pre-plan summary-first struct).

### Proven? (tests exist)
- `tests/test_episodic.c`, `tests/test_deep_memory.c`, `tests/test_e2e_conversation.c` exist — these exercise the pre-existing episodic surface, not the plan's NREM/REM extensions.

### Wired? (called in runtime path / dispatch)
- Existing episodic surface is wired into `src/daemon.c`, `src/agent/episodic.c`.
- No NREM/REM phase wiring on the scheduler; consolidation runs as a single-phase loop.
- No "episodes first, summaries fall back" read-path change visible in the surface.

## Gaps
- The plan's central thesis (NREM/REM two-phase consolidation + verbatim episode storage as ground truth) is unimplemented even though episodic infrastructure exists.
- Verifier-score-on-episode and tool-call-trace-on-episode fields not added.

## Notes
Distinguishing pre-existing W2 background-consolidation work from this
plan's net-new NREM/REM proposal is important: the substrate exists,
the plan's proposed restructuring on top of it has not happened.
