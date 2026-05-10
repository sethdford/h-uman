---
title: "ADR — W10 neural KV replay deferred (metadata-only phase)"
created: 2026-05-10
status: accepted
deciders: engineering
---

# ADR: W10 neural KV replay deferred (metadata-only phase)

## Context

The W10 `neural_kv_cache` table and `hu_kv_cache_*` APIs support opaque `blob` fields intended for provider-specific KV bytes. The agent turn path was extended to **probe** for an existing row (hash + model version) and to **persist** `prompt_token_count` after a successful provider completion.

Provider **short-circuit** (skipping `hu_provider_chat` when a replayable assistant payload exists) requires:

1. A **versioned on-wire format** for `blob` that the active provider can interpret.
2. **Safety rules** (no caching of secrets in system prompts, contact-scoped hashing policy, TTL / invalidation aligned with model upgrades).
3. **Tests** that prove identical semantics vs the non-cached path under `HU_IS_TEST`.

None of the above is complete as of this ADR.

## Decision

**Defer full KV replay and provider short-circuit** until a follow-up project defines the blob contract and lands the tests above.

The shipped behavior remains:

- **Probe** + diagnostic log when a row exists (explicitly **not** labeled as a latency “hit”).
- **Upsert metadata** after successful provider completion.
- **Log** `hu_kv_cache_put` failures instead of discarding errors.

## Consequences

- **Positive:** No risk of serving stale or provider-incompatible “cached” text. Product expectations stay honest.
- **Negative:** No TTFT / cost win from KV until replay ships.
- **Documentation:** `include/human/memory/neural_memory.h` and `agent_turn.c` comments reference this ADR.

## Status

Accepted. Supersede this ADR when replay ships (link replacement ADR from here).
