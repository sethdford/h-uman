---
plan: docs/plans/2026-05-10-w10-neural-memory.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Three new W7 memory backends: KV-cache (compressed activations), reasoning-trace, multimodal blobs. Gated behind `HU_ENABLE_NEURAL_MEMORY`. Persist activations for prompt-caching-style reuse.

## Key Claims (from the plan)
- KV-cache backend
- Reasoning-trace backend
- Multimodal blob backend
- All gated; default OFF

## Evidence

### Implemented? (code exists)
- `src/memory/neural_memory.c` + `include/human/memory/neural_memory.h`
- `src/memory/kv_compressor.c`, `src/memory/kv_deltakv.c`, `src/memory/kv_swan.c`
- `src/agent/kv_cache.c`, `src/agent/kv_prewarm_runner.c`
- `src/memory/multimodal_index.c`
- CMake: `option(HU_ENABLE_NEURAL_MEMORY "W10 neural memory placeholder (schema/ONNX path; default OFF)" OFF)`

### Proven? (tests exist)
- `tests/test_w10_neural_memory.c`
- `tests/test_multimodal_memory.c`

### Wired? (called in runtime path / dispatch)
- `src/agent/agent_turn.c:4769, 4855` — KV cache facade reads
- `src/agent/agent_turn.c:6120, 6136` — neural memory facade + blob facade
- Default OFF — only active in HU_ENABLE_NEURAL_MEMORY matrix builds

## Gaps
- ADR `2026-05-10-w10-kv-replay-deferred.md` exists (per evidence index); KV replay is deferred
- Default-off means production binary does not exercise it

## Notes
Per ADR, full KV replay deferred. Backend skeletons + tests + wiring stubs exist; verdict PARTIAL because production wiring is gated and KV replay is explicitly deferred.
