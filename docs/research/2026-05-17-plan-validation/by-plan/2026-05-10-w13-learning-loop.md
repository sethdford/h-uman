---
plan: docs/plans/2026-05-10-w13-learning-loop.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
`hu_learner_t` vtable (MLX, llama.cpp, CPU-fallback). LoRA + DPO training from W4 verifier flags, W5 persona deltas, W3 case outcomes. Output: adapters loaded by chat-time provider. Closes the M3 gap.

## Key Claims (from the plan)
- `hu_learner_t` vtable
- MLX / llama.cpp / CPU backends
- Adapter loaded into chat path (frontier model bridge)

## Evidence

### Implemented? (code exists)
- `src/agent/lora_training_runner.c`
- `src/ml/` — lora.c, dpo.c, training loops, BPE, GPT, experiment framework
- CMake: `option(HU_ENABLE_LEARNING "Build W13 learning loop (hu_learner_t + CPU/MLX/ggml backends)" OFF)`
- `hu_learner_pending_drain` referenced in CMakeLists.txt (drain API exists when flag on)

### Proven? (tests exist)
- `tests/test_w13_learner.c`

### Wired? (called in runtime path / dispatch)
- Provider dispatcher safety contract pinned by `tests/test_provider_all.c::test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` (per project CLAUDE.md)
- Frontier model bridge plan exists at `docs/plans/2026-05-10-m3-frontier-model-bridge.md` — bridge not yet wired to llama.cpp/MLX
- Adapter training works against reference HUML GPT only (per CLAUDE.md M3 honest status); production chat provider does not yet load LoRA adapters

## Gaps
- LoRA adapter does not yet adapt the frontier chat model — only the reference HUML GPT
- Bridge plan (m3-frontier-model-bridge) outstanding

## Notes
This is the M3 mission's "hardest" challenge per CLAUDE.md. Code shipped + tested + provider-safety-contract pinned; full production wiring (frontier bridge) remains the deliberate gap.
