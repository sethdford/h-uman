---
plan: docs/plans/2026-05-10-w7-type-collision-cleanup.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: N/A
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Phase 0 fix for the type-collision between legacy `hu_memory_t` (vector store in `include/human/memory.h`) and the new W7 dispatch facade. Option A: rename facade to `hu_memory_facade_t`. Status frontmatter: "resolved (Option A — facade rename landed 2026-05-10)".

## Key Claims (from the plan)
- Facade rename to `hu_memory_facade_t`
- Both headers includable in same TU without collision
- `agent_turn.c` can include both transitively without break

## Evidence

### Implemented? (code exists)
- `include/human/memory/memory.h` declares `hu_memory_facade_t`/`hu_memory_facade_*`
- `include/human/memory.h` continues to host legacy `hu_memory_t` for vector store
- `scripts/check-memory-v2-header-collision.sh` enforces no forbidden dual-include in src/ (G2 gate)

### Proven? (tests exist)
- CI gate G2 enforces

### Wired? (called in runtime path / dispatch)
- N/A — header-level rename

## Gaps
- None

## Notes
Doc itself declared "resolved" in frontmatter. Reality matches.
