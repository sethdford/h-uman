---
plan: docs/plans/2026-05-10-w11-inline-self-rag.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Move verification into generation: `<retrieve>`/`<critique>`/`<refuse>` control tokens, atomic noun-phrase claim decomposition, explicit abstention. New `hu_self_rag_t` vtable with v1 heuristic verifier as fallback backend.

## Key Claims (from the plan)
- `hu_self_rag_t` vtable
- Inline backend that uses provider control tokens
- Atomic claim decomposer
- Abstention/refusal path

## Evidence

### Implemented? (code exists)
- `src/agent/self_rag.c` (vtable + dispatch)
- `src/agent/self_rag_inline.c` (inline backend)
- `src/agent/self_rag_atomic.c` (atomic claim decomposer)
- `include/human/agent/self_rag.h`

### Proven? (tests exist)
- `tests/test_w11_self_rag.c`
- `tests/test_w11_abstain_calibration.c` (abstention calibration)

### Wired? (called in runtime path / dispatch)
- Used by `agent_turn.c` verifier path (`verify_mem` reference at 5880)
- W11 inline self-RAG fires during response synthesis

## Gaps
- None major.

## Notes
W11 is the response-time verifier upgrade over W4. Both can coexist (W4 is the fallback).
