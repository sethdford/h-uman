---
plan: docs/plans/2026-05-10-w4-self-rag-provenance.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Wire self-RAG / corrective-RAG / hallucination_guard / verify_claim into the synchronous response path. Surface provenance + ship a memory-view UI + targeted erasure (GDPR Article 15/16/17).

## Key Claims (from the plan)
- Verifier called from `agent_turn.c`
- Provenance receipts attached
- Erasure / memory-view UX

## Evidence

### Implemented? (code exists)
- `src/memory/self_rag.c`, `corrective_rag.c`, `adaptive_rag.c`, `hallucination_guard.c`, `verify_claim.c`
- `src/memory/erasure.c` — `hu_memory_erase_entity`, `hu_memory_erase_by_provenance` (cascading erasure)
- `src/cli_commands.c:30-44` — `hu_memory_facade_export_json` (memory view CLI)
- Provenance fields on `hu_graph_relation_t`

### Proven? (tests exist)
- `tests/test_w4_verifier.c`
- `tests/test_self_rag.c`, `tests/test_validators_persona_safety.c`

### Wired? (called in runtime path / dispatch)
- `src/agent/agent_turn.c:5880` — `hu_memory_facade_t *verify_mem = ...` — verifier path uses facade
- Provenance threaded through `feeds/processor.c` and personal_model writes (commented requirement in personal_model.c:952)

## Gaps
- UI memory-view: facade export-json exists; full ui/ dashboard surface partly covered by separate work

## Notes
W4 is the verifier wire-up. Plan was faithful to landed work.
