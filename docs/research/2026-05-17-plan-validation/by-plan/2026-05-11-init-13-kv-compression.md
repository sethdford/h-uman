---
plan: docs/plans/2026-05-11-init-13-kv-compression.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: NONE
verdict: SHIPPED_UNWIRED
confidence: HIGH
---

## Plan Summary
Compress the W10 neural KV-cache 4–8× at <1% token-distribution quality
loss via two backends — **DeltaKV** (low-rank residual coding) and
**SWAN** (sliding-window pruning with attention-sink retention) —
behind a new vtable, negotiated with the provider via a capability
flag, framed in a versioned blob envelope, refusing to touch any KV
that touches a detected secret.

## Key Claims (from the plan)
- `hu_kv_compressor_t` vtable in `include/human/memory/kv_compressor.h`
- Two backends: `hu_kv_compressor_create_deltakv` and `hu_kv_compressor_create_swan`
- Versioned blob envelope (HUKV magic)
- Provider negotiation via capability flag (consumer: `src/providers/llamacpp.c`)
- Secret-aware refusal path

## Evidence

### Implemented? (code exists)
- `include/human/memory/kv_compressor.h` exists with the vtable.
- `src/memory/kv_compressor.c` — shared envelope read/write helpers.
- `src/memory/kv_swan.c` — SWAN backend, `s_swan_vtable`, `hu_kv_compressor_create_swan()`.
- `src/memory/kv_deltakv.c` — DeltaKV backend, `s_deltakv_vtable`, `hu_kv_compressor_create_deltakv()`.

### Proven? (tests exist)
- `tests/test_kv_compressor.c` (203 LOC) — covers backend creation and the envelope round-trip.

### Wired? (called in runtime path / dispatch)
- Grep for `kv_compressor` consumers in source returned **only the three implementation files** — `kv_compressor.c`, `kv_swan.c`, `kv_deltakv.c`. **No production caller** in providers, memory, neural-memory, or the runtime.
- `src/providers/llamacpp.c` (the plan's named consumer) does not reference `hu_kv_compressor_*`.

## Gaps
- The provider capability-flag negotiation hasn't landed — no `hu_provider_t` field for KV-compression capability, no negotiation hook in the llamacpp provider.
- The secret-aware refusal path's wiring to the secrets scanner is not visible.
- The W10 neural-KV-cache consumer (the upstream W10 plan that this compresses) is not connected.

## Notes
Classic SHIPPED_UNWIRED pattern: the vtable, both backends, the
envelope, and a test exist as a clean module — but nobody calls them.
The plan's "first compression backend that ships ASan-clean and
end-to-end testable" succeeded on the standalone bar; the end-to-end
integration didn't happen.
