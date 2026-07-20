# SOTA Wave B — Measurement (outline)

> Companion to Wave A. Do not start until Wave A exit criteria are green.

**Goal:** Replace architecture claims with published numbers and fail-closed gates.

## Exit criteria

1. LongMemEval score published under `docs/evaluation/` with method notes.
2. LoCoMo P@1 improved vs ≈0.058 baseline **or** explicit documented floor with method.
3. Contact/session isolation regression tests for vector/hybrid retrieval.
4. Blind A/B human n≥30 **or** CI fails closed when gate file says ABSENT and humanness feature is LIVE (see `docs/superpowers/specs/2026-05-31-blind-ab-measurement-gate-design.md`).

## Non-goals

- Full retrieval redesign unless isolation tests require it.
- Claiming memory SOTA in `docs/SOTA_BENCHMARK.md` before numbers exist.
