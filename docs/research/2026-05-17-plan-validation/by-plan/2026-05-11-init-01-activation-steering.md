---
plan: docs/plans/2026-05-11-init-01-activation-steering.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: NONE
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
Adds an optional `hu_provider_vtable_t` activation-steering method that
applies SAE-derived residual-stream steering on on-device providers
(`llamacpp`, `embedded`, future `mlx_qwen3`) and a prompt-side adversarial
weighting fallback on cloud providers. Derives a small steering vector
from `hu_persona_t` + `hu_personal_model_t` style EWMA.

## Key Claims (from the plan)
- New optional vtable method on `hu_provider_t` for steering vector application
- `hu_steering_vector_t` type derived from persona + personal_model style
- Prompt-side adversarial weighting fallback for cloud providers
- SAE feature extraction tied to persona traits (warmth, formality, humor, hedging)
- Tests covering both on-device residual-stream addition and cloud prompt-side fallback

## Evidence

### Implemented? (code exists)
- NONE FOUND. Grep for `steering_vector`, `activation_steer`, `hu_steering`, `steering_apply`, `*sae*`, `*steering*` across `src/`, `include/`, `tests/` returned zero hits beyond the plan document itself.
- `include/human/provider.h` shows existing `load_adapter`/`unload_adapter`/`active_adapter` triple but no `apply_steering_vector` or similar method.

### Proven? (tests exist)
- NONE FOUND. No `tests/test_*steering*.c` or `tests/test_*activation*.c`.

### Wired? (called in runtime path / dispatch)
- N/A — no code to wire.

## Gaps
- Entire C-API surface (§1 of the plan) — vtable additions, prompt-side weighting, SAE feature lookup tables — is unimplemented.
- No persona-to-steering-vector derivation exists.
- No tests pin any contract.

## Notes
Plan status header is "design done" — i.e., it explicitly acknowledges it
is design-only. The plan is well-formed (mapped to D0–D7 gates) but
nothing past D0 has shipped. Critical-path dependency: also presupposes
Init 04 (MLX Qwen3) is the eventual consumer of on-device residual-stream
steering; Init 04 is itself a stub (see init-04 audit).
