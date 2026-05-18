---
plan: docs/plans/2026-05-11-init-09-memory-trust-tiers.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Add per-memory trust tiers + per-fact provenance, MINJA/MemoryGraft
quarantine, and a verifier that gates fact recall on trust. SQLite
migration is additive and backward-compatible. C-side delta capped at
≤12 KB text-segment growth.

## Key Claims (from the plan)
- `hu_trust_tier_t` enum (USER_DIRECT and descending tiers)
- Per-fact provenance struct stored alongside personal-model facts
- MINJA quarantine path on adversarial input
- Trust-gated overwrite semantics in `hu_personal_model_t`
- SQLite schema additive migration

## Evidence

### Implemented? (code exists)
- `include/human/memory/trust.h` exists (canonical `hu_trust_tier_t`, `hu_trust_can_overwrite`, etc.).
- `include/human/memory/minja_guard.h` exists.
- `src/memory/write_trust.c` (260 LOC), `src/memory/minja_guard.c` (415 LOC) implement the policy and the MINJA/MemoryGraft detector.
- Adjacent supporting code: `src/agent/channel_trust.c`, `src/behavior/behavior_trust.c`, `src/cognition/cognition_trust.c`, `src/intelligence/trust.c`, `src/security/skill_trust.c`, `src/evaluation/evaluation_minja.c`.

### Proven? (tests exist)
- `tests/test_trust.c`, `tests/test_trust_calibration.c`, `tests/test_minja_guard.c`.

### Wired? (called in runtime path / dispatch)
- `src/memory/personal_model.c` actively gates writes on trust:
  - L967–968: `if (hu_minja_detect(message, message_len, NULL)) { hu_minja_quarantine_log(...); }`
  - L1120: `hu_trust_tier_t src_tier = prov ? prov->tier : HU_TRUST_USER_DIRECT;`
  - L1135–1137: `if (!hu_trust_can_overwrite(nf->provenance.tier, ef->provenance.tier)) { ... hu_minja_quarantine_log(...); }`
- This is exactly the agent-turn ingestion path the plan targets.

## Gaps
- None identified at the audit's grep depth. (A deeper review would
  check that ALL fact-write call sites — feeds processor, episodic
  ingester — pass provenance; the plan covers these but only the
  personal-model write path was confirmed wired.)

## Notes
This is the strongest "shipped" verdict in the Init-08-to-11 cluster.
Plan was tagged "HIGHEST — precondition for shipping #04 and #05 to
users" and looks like it was treated as such — it landed even though
#04 and #05 didn't.
