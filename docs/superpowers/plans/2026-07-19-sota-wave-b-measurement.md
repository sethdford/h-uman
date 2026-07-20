# SOTA Wave B — Measurement

> Status: **implemented** on `feat/sota-waves-a-b-c` (2026-07-19).

**Goal:** Replace architecture claims with published numbers and fail-closed gates.

## Exit criteria

1. [x] LongMemEval method notes: `docs/evaluation/longmemeval-method.md` (+ baseline.json scores).
2. [x] LoCoMo floor documented: `docs/evaluation/locomo-method.md` (P@1 = 0.057717; improve later with same method).
3. [x] Contact/session isolation: `hu_retrieval_options_t` namespace + `tests/test_retrieval_contact_isolation.c`.
4. [x] Blind A/B fail-closed: `scripts/check_capability_gates.py` rejects LIVE when human ABSENT / n&lt;30. Human n≥30 sheet still **ABSENT** (manual outer loop).

## Non-goals (unchanged)

- Full retrieval redesign unless isolation tests require it.
- Claiming memory SOTA in `docs/SOTA_BENCHMARK.md` before numbers improve.
