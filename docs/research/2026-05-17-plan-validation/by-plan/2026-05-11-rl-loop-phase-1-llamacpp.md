---
plan: docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md
auditor: group-9-adversarial-rl
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Phase 1 replaces the `HU_ERR_NOT_SUPPORTED` stub in `llamacpp_chat_with_system` with a real Metal-accelerated, in-process Gemma-3-4B-it inference path. Three new testable modules (sampling, KV cache, decode loop) plus vendored llama.cpp pinned at `b9055`. Ends with a 20-prompt stock-Gemma sanity gate.

## Key Claims (from the plan)
- Claim 1: New three-module decomposition under `src/providers/` (sampling ~200 LOC, KV cache ~300, decode ~250) with `llamacpp_chat_with_system` as thin orchestrator.
- Claim 2: Vendored llama.cpp at `b9055`; SHA-pinned and verified by `scripts/verify-llamacpp-pin.sh`.
- Claim 3: CMake knob `HU_LLAMACPP_METAL` default ON on `__APPLE__`; CMake preset `rl_sota`.
- Claim 4: `vtable.warmup`, `vtable.load_adapter`/`unload_adapter` via `llama_adapter_lora_init` + `llama_set_adapters_lora`.
- Claim 5: 20-prompt sanity gate via `scripts/run-gemma-sanity-gate.sh` + `tests/fixtures/gemma_sanity_gate_prompts.json`.

### Implemented? (code exists)
- `src/providers/llamacpp.c`, `src/providers/llamacpp_decode.c`, `src/providers/llamacpp_kvcache.c`, `src/providers/llamacpp_sampling.c`.
- `include/human/providers/llamacpp_kvcache.h`.
- `scripts/llamacpp-serve.sh`, `scripts/verify-llamacpp-pin.sh`, `scripts/fetch-gemma.sh`, `scripts/run-gemma-sanity-gate.sh`, `scripts/bench-gemma-perf.py`.
- `third_party/llama.cpp.sha256` (SHA pin).

### Proven? (tests exist)
- `tests/test_llamacpp_decode.c`
- `tests/test_llamacpp_kvcache.c`
- `tests/test_llamacpp_lora_hotswap.c`
- `tests/test_llamacpp_provider.c`
- `tests/test_llamacpp_chat_metal.c`
- `tests/test_llamacpp_sampling.c`
- `tests/test_llamacpp_factory_config.c`
- Sanity gate 20/20 (per umbrella status table).

### Wired? (called in runtime path / dispatch)
- `human chat --provider llamacpp --model gemma-3-4b-it-Q4_K_M` is the user-visible CLI path (DoD-3 PASS_WITH_NOTES = sanity gate 18/20 pass-bar).
- Provider factory wires `"llamacpp"` to `hu_llamacpp_provider_create`.
- Phase 2's MLX subprocess hot-loads `.safetensors` via this provider's `load_adapter` vtable.

## Gaps
- Sanity gate ships with `PASS_BAR=18/20` per ship contract (not the 20/20 ceiling); umbrella table reports 20/20 PASS but ship contract corrected this.

## Notes
Tag `rl-sota-phase-1-complete` exists. dev preset 9739/9739 tests pass; `rl_sota` preset 10140/10140 tests pass under ASan. Sprint-auditor PASS_WITH_NOTES on first pass; followups addressed in tag commit.
