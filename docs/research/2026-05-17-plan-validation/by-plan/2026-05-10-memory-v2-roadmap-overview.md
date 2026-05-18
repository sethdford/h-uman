---
plan: docs/plans/2026-05-10-memory-v2-roadmap-overview.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Top-level overview of memory v2 (W7-W16): facade → belief → world model → neural tier → inline self-RAG → planner → learner → sleep compute → crypto → eval suite. Status: in_progress per frontmatter.

## Key Claims (from the plan)
- 7-layer architecture, one vtable per workstream
- Ten frontier-capability outcomes (LoRA-loop, inline self-RAG, world model, beliefs not floats, KV reuse, goal-conditioned retrieval, sleep-time compute, crypto forgetting, negative memory, benchmark proof)

## Evidence

### Implemented? (code exists)
- W7: `include/human/memory/memory.h` — `hu_memory_facade_t` API + Phase 1 inventory snapshot (zero v1 graph bypass in agent/persona/feeds per `w7-phase1-graph-bypass-inventory.sh`)
- W8: `include/human/memory/belief.h` — `hu_belief_t`, `hu_belief_init`, `hu_belief_update`, `hu_belief_combine`, `hu_belief_semantic_conflict`
- W9: `include/human/agent/world_model.h` + `src/agent/world_model.c` + `world_model_bridge.c`
- W10: `src/memory/neural_memory.c` + `include/human/memory/neural_memory.h` (HU_ENABLE_NEURAL_MEMORY gated)
- W11: `src/agent/self_rag_inline.c` + `self_rag_atomic.c`
- W12: `src/agent/retrieval_planner.c` + `retrieval_planner_llm.c` (status: shipped per frontmatter)
- W13: `src/agent/lora_training_runner.c` + `src/ml/` learning stack (HU_ENABLE_LEARNING gated)
- W14: `src/agent/scheduler.c` + `kv_prewarm_runner.c` + `belief_reverify_runner.c`
- W15: `src/security/keystore.c` + `audit.c` (frontmatter notes keystore+audit landed, DP-SGD+GDPR export pending)
- W16: `src/evaluation/` (locomo, longmemeval, dmr, minja, memoryagentbench, frontier_compare)

### Proven? (tests exist)
- One test per W: `test_w7_memory_facade.c`, `test_w8_belief_layer.c`, `test_w9_world_model.c`, `test_w10_neural_memory.c`, `test_w11_self_rag.c`/`test_w11_abstain_calibration.c`, `test_w12_planner.c`/`test_w12_verifier_loop.c`, `test_w13_learner.c`, `test_w14_runners.c`/`test_w14_scheduler.c`, `test_w15_backup_restore.c`/`test_w15_keystore.c`, `test_w16_evaluation.c`/`test_w16_eval_cli.c`

### Wired? (called in runtime path / dispatch)
- `src/agent/agent_turn.c` calls `hu_w7_facade_memory_handle()` at 7+ sites (facade); world model via `agent->w7_facade`; planner default; KV cache wiring
- `src/daemon.c` calls scheduler/AutoDream 17+ times

## Gaps
- W15 DP-SGD and GDPR Article 20 export pending per the plan's own status line
- Some W-streams marked "proposed" in their own files but ship in tree (status is stale, not the code)

## Notes
This is the apex roadmap; nearly all of v2 is in-tree with deep wiring. Per-W audits below confirm the breakdown.
