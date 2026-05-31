---
plan: docs/plans/2026-05-10-w7-memory-facade.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Single `hu_memory_facade_t` read/write/erase surface for structured memory kinds; backends are vtables keyed on `hu_memory_kind_t`. Phase 0 type-collision fix landed (rename to `hu_memory_facade_t`); Phase 1 inventory of bypasses is zero in agent/persona/feeds.

## Key Claims (from the plan)
- `hu_memory_facade_open`/`close`/`read`/`write`/`erase` API
- Backends: graph, vector, persona deltas, cross-edges, cases, quarantine, neural memory
- Every agent consumer goes through facade

## Evidence

### Implemented? (code exists)
- `include/human/memory/memory.h` — full `hu_memory_facade_t` API: open/close/export_json/read/write
- `HU_MEMORY_REL_VERIFIER_SCAN` sentinel for verifier-shaped reads
- `hu_memory_case_payload` for case kind
- `hu_w7_facade_*` integration helpers used by agent

### Proven? (tests exist)
- `tests/test_w7_memory_facade.c`
- `tests/test_v2_wiring_e2e.c`

### Wired? (called in runtime path / dispatch)
- `src/agent/agent_turn.c` calls `hu_w7_facade_memory_handle(agent->w7_facade)` at lines 3875, 4769, 4855, 5880, 6120, 6136
- `src/app/cli_commands.c:539-544` uses facade for memory export
- `src/daemon.c:3248, 3263` uses facade in AutoDream contexts
- Phase 1 inventory: zero direct `hu_graph_*` API calls remain under `src/agent/`, `src/persona/`, `src/feeds/`

## Gaps
- None — bypass count is zero per `scripts/w7-phase1-graph-bypass-inventory.sh`

## Notes
W7 is the layer-1 foundation of the v2 stack. Phase 0 collision (with legacy `hu_memory_t` for vector store) resolved by rename. Plan was high-risk; landed cleanly.
