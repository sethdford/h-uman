---
plan: docs/plans/2026-03-21-dual-process-cognition.md
auditor: group-5-cognition-twin
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Introduce System-1 / System-2 / System-3 mode switching: a dispatcher classifies each turn (FAST/SLOW/DEEP) and a per-mode budget gates tool loops, retrieval depth, and reflection. Status in frontmatter says "proposed" but implementation has landed.

## Key Claims (from the plan)
- Claim 1: `hu_cognition_mode_t` enum + dispatcher API
- Claim 2: Per-mode budgets (tool iterations, retrieval depth, etc.)
- Claim 3: Dispatch invoked early in `agent_turn`/`agent_stream`
- Claim 4: Mode name exposed for logs/telemetry
- Claim 5: Tests in `tests/test_cognition.c`

## Evidence

### Implemented? (code exists)
- `src/cognition/dual_process.c:78,113,184,226,281` — full module (288 LOC) with `hu_cognition_dispatch`, `hu_cognition_get_budget`, `hu_cognition_mode_name`, init/cleanup.
- `include/human/cognition/dual_process.h` — public API present.

### Proven? (tests exist)
- `tests/test_dual_process.c` — multiple scenarios covering dispatch input → mode mapping (FAST/SLOW/DEEP) with at least 6 distinct dispatch test cases (verified by grep on `hu_cognition_dispatch` callsites).

### Wired? (called in runtime path / dispatch)
- `src/agent/agent_turn.c:1050-1217` — `hu_cognition_get_budget`, `hu_cognition_dispatch_input_t`, `hu_cognition_dispatch`, `hu_cognition_mode_name` all called inline in the turn loop. Result stored in `agent->infra.current_cognition_mode`.
- `src/agent/agent_stream.c:962` — mode name surfaced for streaming telemetry.
- `src/agent/agent_turn.c:3494` — mode name referenced again later in turn for diagnostics.

## Gaps
- Plan frontmatter still says `status: proposed` despite full shipment — stale metadata.
- Tests file name in plan (`test_cognition.c`) doesn't exist; actual file is `test_dual_process.c` — harmless naming drift.

## Notes
Genuinely shipped and wired. Plan-status drift only.
