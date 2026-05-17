---
plan: docs/plans/2026-05-10-w8-belief-layer.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Replace scalar `confidence: float` with `hu_belief_t` (mean, variance, provenance). LLM-judge semantic conflict detection. Hyperedges for n-ary facts.

## Key Claims (from the plan)
- `hu_belief_t` struct with mean/variance/provenance
- LLM-judge conflict detector for paraphrased contradictions
- Hyperedges for n-ary facts

## Evidence

### Implemented? (code exists)
- `include/human/memory/belief.h`:
  - `typedef struct hu_belief { ... } hu_belief_t;`
  - `hu_belief_init`, `hu_belief_update`, `hu_belief_combine`, `hu_belief_significantly_disagrees`
  - `hu_belief_initial_variance_for_provenance`
  - `hu_belief_conflict_t` enum + `hu_belief_semantic_conflict` (deterministic) + `hu_belief_semantic_conflict_with_provider` (LLM-judge)
  - `hu_provenance_atom` struct
- `src/memory/belief.c`
- `include/human/memory/hyperedge.h` + `src/memory/hyperedge.c` — n-ary fact support
- `hu_graph_relation_t.variance` field (in graph.h)

### Proven? (tests exist)
- `tests/test_w8_belief_layer.c`

### Wired? (called in runtime path / dispatch)
- `hu_graph_upsert_relation_ex` calls `hu_belief_initial_variance_for_provenance` on every relation write
- W11 / agent self-RAG paths consume belief variance for abstention threshold

## Gaps
- None major

## Notes
Beliefs are first-class. The deterministic + provider-backed conflict detectors are both present. Hyperedges ship.
