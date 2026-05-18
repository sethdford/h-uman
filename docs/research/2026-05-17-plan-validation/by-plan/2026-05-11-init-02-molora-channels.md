---
plan: docs/plans/2026-05-11-init-02-molora-channels.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: NONE
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
Adds a Mixture-of-LoRA-Experts (MoLoRA) per-channel persona routing system:
one base model + 4–8 small LoRA experts (one per channel + persona macro-mode),
gated by a learned ≤16K-param MLP router that picks 1–3 active experts per turn
based on (channel, message_class, macro_mode). Uses llama.cpp's
`llama_set_adapters_lora()` array hook.

## Key Claims (from the plan)
- `HU_MOLORA_MAX_ACTIVE = 3` constant and `hu_molora_*` C API
- LoRA expert slots [0..7]; slot 0 = persona macro-mode, slots 1-6 = channels, slot 7 = TTT (init #05)
- Learned MLP router consuming (channel_id, message_class, macro_mode)
- W14 idle scheduler retrains the router offline
- Integration with llama.cpp's `llama_set_adapters_lora(ctx, A[], n, w[])`

## Evidence

### Implemented? (code exists)
- NONE FOUND. Grep for `HU_MOLORA`, `hu_molora`, `MoLoRA`, `molora_router`, `llama_set_adapters_lora` across `src/`, `include/`, `tests/` returned zero hits beyond the plan document.
- No `src/ml/molora*.c`, no `include/human/ml/molora.h`.
- llamacpp provider (`src/providers/llamacpp.c`) does not reference the multi-adapter array API.

### Proven? (tests exist)
- NONE FOUND. No `tests/test_*molora*.c`, no `tests/test_*mole*.c`.

### Wired? (called in runtime path / dispatch)
- N/A — no code to wire.

## Gaps
- Entire MoLoRA C API, router, and llama.cpp multi-adapter integration unimplemented.
- W14 idle-scheduler retrain hook not present (it would need to attach to consolidation scheduler).
- Persona overlay → expert routing path absent.

## Notes
The plan is explicitly status "design" and depends on Init 04 (MLX Qwen3) for the
on-device frontier path and Init 05 (TTT) for slot 7. Both upstream initiatives
are themselves stubs/not started; MoLoRA can't ship until those land.
