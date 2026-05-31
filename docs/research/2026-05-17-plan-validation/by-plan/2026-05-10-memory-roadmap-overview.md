---
plan: docs/plans/2026-05-10-memory-roadmap-overview.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Umbrella roadmap sequencing W1-W6 (bitemporal foundation, background consolidation, multi-graph topology, self-RAG provenance, agent-writable persona, eval+MemRL+red-team) plus a skills pack. Each is its own branch and PR.

## Key Claims (from the plan)
- W1-W6 each leaves a tested vertical slice
- Skills pack ships in parallel with W1
- Cross-cutting principles: one concern per branch, vtable discipline, HU_IS_TEST guards

## Evidence

### Implemented? (code exists)
- W1: `include/human/memory/graph.h` shows bitemporal `event_start`/`event_end`, `provenance`, `supersedes_id` fields, `hu_belief_t` field
- W2: `src/memory/consolidation_engine.c`, `src/agent/simulation/autodream.c` ship; `autodream_runs` table
- W3: `src/memory/cross_graph.c`, `src/memory/emotional_graph.c`, `src/memory/contact_graph.c`, `src/memory/episodic.c`, `src/memory/relational_episode.c`, `src/agent/case_based.h`
- W4: `src/memory/self_rag.c`, `src/memory/corrective_rag.c`, `src/memory/adaptive_rag.c`, `src/memory/hallucination_guard.c`, `src/memory/verify_claim.c`, `src/memory/erasure.c`
- W5: `include/human/persona/persona_deltas.h`, `include/human/persona/delta_observer.h`
- W6: `src/evaluation/evaluation_locomo.c`, `evaluation_minja.c`, `evaluation_memoryagentbench.c`, `evaluation_dmr.c`

### Proven? (tests exist)
- `test_w1_bitemporal.c`, `test_w2_autodream.c`, `test_w3_multigraph.c`, `test_w4_verifier.c`, `test_w5_persona_deltas.c`, `test_w6_e2e_adversarial.c` all present

### Wired? (called in runtime path / dispatch)
- `src/agent/agent_turn.c` references W7 facade (7 sites) + world model + self_rag + planner + KV cache
- `src/daemon.c` references scheduler (17 sites) for AutoDream + sleep compute

## Gaps
- None at the overview level — the roadmap itself is shipped via W1-W6.

## Notes
Overview document; status v2 evolved this into the W7-W16 v2 roadmap. v1 (W1-W6) ships per the explicit "v1 (W1–W6, merged May 2026)" note in the memory-v2-roadmap-overview.
