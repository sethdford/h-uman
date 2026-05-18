---
plan: docs/plans/2026-05-11-adversarial-review-critic.md
auditor: group-9-adversarial-rl
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Critic-agent adversarial review of 14 SOTA-2026 initiative designs, returning 4 BLOCKERs and 8 MAJOR findings that needed to land before any S1 implementation dispatch.

## Key Claims (from the plan)
- Claim 1 (B1): Trust-tier ordinal direction inverted between init-09 and init-10 — must be locked.
- Claim 2 (B2): Init-09 migration would upgrade poisoned facts to FIRST_PARTY trust.
- Claim 3 (B3): `hu_personal_model_ingest` signature change has ≥4 unpatched call sites.
- Claim 4 (B4): `hu_episode_t` already defined incompatibly in two headers — live ODR violation.

## Evidence

### Implemented? (code exists)
- B1: Trust-tier ordinal convention locked at `docs/plans/2026-05-11-sota-2026-massive-team-program.md` and consumed in `src/memory/personal_model.c` (USER_DIRECT=4, THIRD_PARTY=1).
- B2: `src/memory/engines/{sqlite,markdown,postgres,sqlite_lucid}.c` migrations default `trust_tier = HU_TRUST_THIRD_PARTY` (1) on pre-existing rows — verified at e.g. `sqlite.c` migration M5 comment.
- B3: All 4 call sites take the new `hu_provenance_t *` arg (`agent_stream.c:381,2576`, `agent_turn.c:962`, `personal_model.c:1904`).
- B4: `hu_episode_t` renamed; canonical comment in `include/human/memory/episodic.h:13-15`. The two real types are `hu_session_episode_t` (agent/episodic.h:25) and `hu_deep_episode_t` (memory/deep_memory.h:27).

### Proven? (tests exist)
- B1/B2/B3: trust-tier consumers tested across `tests/test_memory_*.c`, `tests/test_personal_model_*.c`.
- B4: ODR fix is structural (header rename) — no explicit pinning test, but the build wouldn't link without it.
- MINJA broadening (M1) — `src/memory/minja_guard.c` contains ~5 pattern tiers, comment cites init-09 §2.6.

### Wired? (called in runtime path / dispatch)
- Trust tiers consumed in `personal_model.c` decision logic (`if (src_tier <= HU_TRUST_THIRD_PARTY)` gating).
- Provenance flows through agent_stream → personal_model → memory engines.

## Gaps
- M2-M8 are deferred-to-S2 mitigations on initiatives that didn't ship in S1 (Apple FM, Qwen3, TTT, MoLORA). They remain in the design docs as revision tickets.

## Notes
This is the upstream peer of `adversarial-review-synthesis.md`. All 4 BLOCKERs closed before any RL phase dispatched. The synthesis doc tracks the actions and they're all visible in the codebase.
