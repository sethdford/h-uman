---
plan: docs/plans/2026-05-11-adversarial-review-api-contracts.md
auditor: group-9-adversarial-rl
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Adversarial API-contract review of the 14-initiative SOTA-2026 design fleet, surfacing 6 ABI-breaking findings and 37 name-collision call sites that would have failed at first compile.

## Key Claims (from the plan)
- Claim 1: `hu_personal_model_ingest` gains a required new `prov` parameter; 4 live call sites (not 2) must be patched.
- Claim 2: `hu_job_kind_t` enum allocation table required — 4 initiatives racing for the same ordinal.
- Claim 3: `hu_episode_t` ODR violation across `agent/episodic.h`, `memory/deep_memory.h`, `memory/episodic.h` must be renamed (W7 slice).
- Claim 4: Init #12 removes `hu_mcp_host_*` symbol family with 5 unguarded call sites in `src/app/main.c`.

## Evidence

### Implemented? (code exists)
- `src/memory/personal_model.c:925` — `hu_personal_model_ingest` signature now takes `const hu_provenance_t *prov` (FULL).
- All 4 call sites patched: `src/agent/agent_stream.c:381,2576`, `src/agent/agent_turn.c:962`, `src/memory/personal_model.c:1904`.
- `include/human/memory/episodic.h:13-15` documents that `hu_episode_t` is **reserved and unused**; renamed to `hu_session_episode_t` and `hu_deep_episode_t` in the two real consumers (deep_memory.h:27 / agent/episodic.h:25). ODR violation resolved.
- Trust-tier ordinals locked in `docs/plans/2026-05-11-sota-2026-massive-team-program.md` and used consistently in `src/memory/personal_model.c` (USER_DIRECT=4, THIRD_PARTY=1).

### Proven? (tests exist)
- `tests/test_dpo_judge_naming.c` pins the `hu_dpo_train_step` → `hu_dpo_judge_step` rename shim.
- `tests/test_personal_model_atomic_save.c` exists for ingest path.
- Trust-tier tests exist throughout `tests/test_memory_*.c` (third-party defaults verified).

### Wired? (called in runtime path / dispatch)
- All 4 ingest call sites are live in the runtime path (agent_stream/turn).
- Trust-tier enum is consumed across `src/memory/personal_model.c`, `src/memory/engines/{sqlite,markdown,postgres,sqlite_lucid}.c`.

## Gaps
- None blocking. Init #12 (MCP) was deferred and its `hu_mcp_host_*` rename is tracked separately (not in scope of this audit's S1 dispatch).

## Notes
This review is the upstream peer of `2026-05-11-adversarial-review-synthesis.md`. Findings were absorbed into the master plan and per-initiative design docs before any S1 dispatch.
