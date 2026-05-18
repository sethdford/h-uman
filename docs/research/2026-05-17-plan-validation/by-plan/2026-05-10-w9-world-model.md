---
plan: docs/plans/2026-05-10-w9-world-model.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Replace fan-out persona/graph/emotion/contact calls with one `hu_world_model_load(memory, contact, &out)`. Adds theory-of-mind and negative-memory cells.

## Key Claims (from the plan)
- `hu_world_model_t` aggregate
- Theory-of-mind: what does the user believe about me?
- Negative memory: what should I never say here?

## Evidence

### Implemented? (code exists)
- `include/human/agent/world_model.h` + `src/agent/world_model.c`
- `include/human/agent/world_model_bridge.h` + `src/agent/world_model_bridge.c`
- `include/human/agent/theory_of_mind.h` + `src/agent/theory_of_mind.c`
- `include/human/agent/tom_scenario.h` + `src/agent/tom_scenario.c`

### Proven? (tests exist)
- `tests/test_w9_world_model.c`
- `tests/test_world_context.c`, `tests/test_world_model_bridge.c`, `tests/test_world_model_graph.c`, `tests/test_world_simulation.c`, `tests/test_inner_world.c`

### Wired? (called in runtime path / dispatch)
- `src/agent/agent_turn.c` references world model through `agent->w7_facade` (which composes world-model + persona + emotion calls behind facade)
- World-model load is invoked in context building per agent_turn.c paths

## Gaps
- None major.

## Notes
This W also depends on W7 facade (shipped). Negative memory + ToM cells exist.
