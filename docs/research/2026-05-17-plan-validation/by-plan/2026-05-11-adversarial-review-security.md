---
plan: docs/plans/2026-05-11-adversarial-review-security.md
auditor: group-9-adversarial-rl
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
Adversarial security review of 7 SOTA-2026 initiatives plus 2 pre-existing P0 violations in main. 4 CRITICAL findings (mathematically broken SECAGG, env-var code-injection path, missing TTT trust-tier gate, MCP peer-driven LoRA replacement) plus 10 MINJA-bypass categories.

## Key Claims (from the plan)
- Claim 1 (S-CRIT-1): Init #08 SECAGG over GF(2^8) is mathematically wrong — XOR-aggregating quantised gradients.
- Claim 2 (S-CRIT-2): `$HUMAN_MLX_QWEN3_HELPER` env-var override is a code-injection path; must gate to `HU_IS_TEST` only.
- Claim 3 (S-CRIT-3): TTT (init #05) has no trust-tier gate before #09 ships.
- Claim 4 (S-CRIT-4): MCP `call_tool` exposure (init #12) allows peer-driven `replace_lora_adapter` hijack.
- Claim 5 (S-MAJOR): MINJA detector has 10 bypass categories — needs ≥30 patterns + NFKC + leetspeak + pending_facts.log.

## Evidence

### Implemented? (code exists)
- MINJA broadening (S-MAJOR): `src/memory/minja_guard.c` has tiered pattern arrays (INSTRUCTION, IDENTITY, CAPABILITY) cited at `init-09 §2.6`. Pattern set is broader than the original 10, though I did not count exactly.
- S-CRIT-1 (SECAGG): no `src/learning/federated*` or `src/ml/secagg*` files exist — Init #08 was deferred (no implementation, no exposure).
- S-CRIT-2 (qwen3 env-var): no `HUMAN_MLX_QWEN3_HELPER` symbol found in `src/` or `include/` — Init #04 was deferred (no qwen3 provider shipped).
- S-CRIT-3 (TTT trust gate): no TTT module shipped; trust-tier infrastructure exists (USER_DIRECT/THIRD_PARTY in personal_model.c) so the gate is buildable when TTT lands.
- S-CRIT-4 (MCP write-tools): MCP server mode not in current shipped surface — Init #12 deferred.

### Proven? (tests exist)
- MINJA: detection tested in `tests/test_minja_guard*.c` (presence verified in tree).
- HU_PERM_DENY (audit-followup, related): `src/permission.c` returns HU_PERM_DENY for unknown tools; pinned by `tests/test_permission_*.c`.

### Wired? (called in runtime path / dispatch)
- MINJA guard called from memory ingest path via the trust-tier downgrade logic.
- The deferred initiatives (08, 04 qwen3, 05, 12) are not wired because they did not ship — the security findings function as design-doc gates.

## Gaps
- Init #08 (federated LoRA) — design-doc revision logged but not implemented.
- Init #04 (qwen3 provider) — not in current ml/ tree.
- Init #05 (TTT) — not shipped.
- Init #12 (MCP server mode) — not shipped.
- These are appropriate "design-doc revision required" outcomes for deferred work; not blocking ships.

## Notes
The MOST-EMBARRASSING scenarios (S-CRIT-1 through S-CRIT-4) all target initiatives that did NOT ship. The one shipped MINJA broadening (S-MAJOR) landed in `minja_guard.c`. Pre-existing P0 violations in main referenced at §0 of the plan are covered by the separate audit-followups directory.
