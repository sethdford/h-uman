---
plan: docs/plans/2026-05-10-w1-bitemporal-foundation.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Bitemporal edges (event_start/event_end separate from first_seen/last_seen), write-time conflict resolver, write-trust score, supersession on contradiction, LoCoMo skeleton.

## Key Claims (from the plan)
- `hu_graph_relation_t` gets bitemporal columns + provenance
- Write-time conflict detection on every fact write
- LoCoMo eval skeleton

## Evidence

### Implemented? (code exists)
- `include/human/memory/graph.h:56-122` — `hu_graph_relation_t` has `event_start`, `event_end`, `supersedes_id`, `provenance`, `provenance_len`, `confidence`, `variance` fields
- `hu_graph_upsert_relation_ex` derives belief variance from provenance: `hu_belief_initial_variance_for_provenance`
- `src/memory/graph.c` exposes `hu_graph_detect_conflict`, `hu_graph_reconsolidate`
- `src/memory/write_trust.c` + `include/human/memory/write_trust.h` — write-trust score module

### Proven? (tests exist)
- `tests/test_w1_bitemporal.c` exists

### Wired? (called in runtime path / dispatch)
- `src/memory/fact_extract_llm.c:111` passes `hu_provenance_t` on every fact write
- `src/memory/minja_guard.c:374` consumes provenance for poisoning defense
- `src/memory/personal_model.c:927/952` requires non-NULL provenance from `feeds/processor.c`

## Gaps
- Plan also mentions LoCoMo skeleton — covered by W16 evaluation_locomo and shipped in `src/evaluation/`

## Notes
W1 is the bedrock the entire W-series depends on; ships and wired. Status frontmatter says "proposed" but code reality is shipped.
