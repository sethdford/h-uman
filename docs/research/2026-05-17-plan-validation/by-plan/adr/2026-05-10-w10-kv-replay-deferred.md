---
plan: docs/plans/adr/2026-05-10-w10-kv-replay-deferred.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
ADR documenting the decision to DEFER full KV cache replay / provider short-circuit
until a follow-up project lands the versioned blob format, safety rules, and parity
tests. Shipped behavior: probe + metadata persist + log on failure — no short-circuit.

## Key Claims (from the plan)
- Claim 1: NO short-circuit of `hu_provider_chat` — providers always called for completion
- Claim 2: Probe-on-hit emits diagnostic log, NOT labeled as a "hit"
- Claim 3: Metadata upserted after successful provider completion (`prompt_token_count`)
- Claim 4: `hu_kv_cache_put` failures logged, not silently discarded
- Claim 5: Header `include/human/memory/neural_memory.h` and `agent_turn.c` reference this ADR

## Evidence

### Implemented? (code exists)
- `include/human/memory/neural_memory.h:33-86` — `hu_kv_cache_entry_t` struct, get/put/invalidate
  APIs present, NO short-circuit decision API
- `src/agent/agent_turn.c:4448` — kv_cache manager initialization
- `grep -n "kv_replay\|kv_short_circuit\|short.circuit.*provider" src/agent/agent_turn.c
  include/human/memory/neural_memory.h` returns 0 hits — short-circuit NOT implemented,
  consistent with the decision to defer

### Proven? (tests exist)
- KV cache get/put/invalidate functions exist; their tests are part of the broader
  memory test suite. No "replay short-circuits provider" test (correctly absent).

### Wired? (called in runtime path / dispatch)
- Probe + metadata persist path is wired in `agent_turn.c` per the decision

## Gaps
- None. ADR is honored by NOT implementing short-circuit.

## Notes
This is an ADR, not an implementation plan. The verdict SHIPPED means "the decision
is honored" — code does NOT contain the deferred feature. When KV replay eventually
ships, this ADR should be superseded.
