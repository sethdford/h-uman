#ifndef HU_LLAMACPP_KVCACHE_H
#define HU_LLAMACPP_KVCACHE_H

/*
 * Phase 1 (RL SOTA) → Phase 2 (Gemma throughput program) — KV-cache
 * index for the in-process llama.cpp provider.
 *
 * The actual KV cache lives inside llama_context (managed by upstream).
 * What this module owns is the bookkeeping that lets chat_with_system
 * decide whether some recently-decoded system-prefix tokens are still
 * resident in the llama_context's KV cache and can be reused.
 *
 * Phase 1 design (1-slot): tracked the most-recent system-prompt hash
 * only — every persona / channel switch evicted the prior entry, so
 * multi-persona conversations thrashed.
 *
 * Phase 2 design (this header): the cache is an N-slot LRU map keyed
 * on system_prompt FNV1a hash. N=4 is sized for the common case of
 * 2-4 persona overlays alive at once (one per Tier-1 channel). Each
 * slot tracks (hash, n_past_system, last_used_tick). lookup_system
 * does a linear scan; record_system either updates a matching slot,
 * fills an empty one, or evicts the slot with the smallest
 * last_used_tick. Hit/miss counters from Phase 0.3 are preserved
 * as lifetime telemetry — they live above the slot array, not per-
 * slot, so reset() and LoRA hot-swap don't zero them out.
 *
 * Phase 2 explicitly does NOT change the cache key from system_prompt
 * bytes to a structural (persona_id, channel, ...) key. The bytes-
 * hash is a defensive correctness choice: it cannot serve KV computed
 * from a different prompt. The structural-key + decode-skip
 * optimizations (the "actual TTFT win") wait on baseline data and
 * are tracked as Phase 2b / 2c in the umbrella plan.
 *
 * Adapter load/unload (LoRA hot-swap) MUST call
 * hu_llamacpp_kvcache_reset() because per-token KV depends on the
 * model's effective weights.
 */

#include "human/core/error.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Phase 2 — fixed cap chosen for stack-friendly embedding inside the
 * provider context. 4 slots covers the Tier-1 channel count (telegram,
 * discord, imessage, slack) so per-channel personas can coexist without
 * thrash. Cranking this requires zero per-call cost beyond the linear
 * scan; the LRU tick stays a single uint64 regardless of N. */
#define HU_LLAMACPP_KVCACHE_SLOTS 4

typedef struct hu_llamacpp_kvcache_slot {
    uint64_t system_prompt_hash; /* 0 -> empty slot */
    int32_t n_past_system;       /* token count consumed by the prefix */
    uint64_t last_used_tick;     /* per-cache monotonic; 0 when empty */
} hu_llamacpp_kvcache_slot_t;

typedef struct hu_llamacpp_kvcache {
    hu_llamacpp_kvcache_slot_t slots[HU_LLAMACPP_KVCACHE_SLOTS];
    /* Monotonic counter incremented on every lookup-hit and every
     * record. Wraparound is theoretical at uint64 scale (>500 years
     * at 1 billion ops/sec). */
    uint64_t tick;
    /* Phase 0.3 — hit-rate telemetry. Incremented inside lookup_system.
     * Atomic so a future /health endpoint or metrics scraper on a
     * different thread can read without tearing. The chat path itself
     * is already serialized per provider context (see compatible.c
     * g_compatible_chat_lock for the analogous discipline), so these
     * are write-mostly from one thread + read-rarely from another. */
    _Atomic uint64_t hits;
    _Atomic uint64_t misses;
} hu_llamacpp_kvcache_t;

hu_error_t hu_llamacpp_kvcache_init(hu_llamacpp_kvcache_t *cache);

/* Record that `system_prompt` was decoded into the llama_context and the
 * cursor is now at `n_past_system`. Overwrites any prior slot. */
hu_error_t hu_llamacpp_kvcache_record_system(hu_llamacpp_kvcache_t *cache,
                                             const char *system_prompt, size_t system_prompt_len,
                                             int32_t n_past_system);

/* Look up `system_prompt`. On hit: HU_OK + *out_n_past_system set.
 * On miss: HU_ERR_NOT_FOUND, *out_n_past_system left untouched. */
hu_error_t hu_llamacpp_kvcache_lookup_system(hu_llamacpp_kvcache_t *cache,
                                             const char *system_prompt, size_t system_prompt_len,
                                             int32_t *out_n_past_system);

void hu_llamacpp_kvcache_reset(hu_llamacpp_kvcache_t *cache);
void hu_llamacpp_kvcache_free(hu_llamacpp_kvcache_t *cache);

uint64_t hu_llamacpp_kvcache_fnv1a(const char *data, size_t len);

/* Phase 0.3 — telemetry getters. Return 0 if `cache` is NULL so a future
 * metrics aggregator can iterate over providers without null-checking.
 * Reads are relaxed-ordered atomics: counters are monotonically
 * incrementing, so a torn read would only ever produce a stale-low
 * value — never a value larger than the true count. */
uint64_t hu_llamacpp_kvcache_hits(const hu_llamacpp_kvcache_t *cache);
uint64_t hu_llamacpp_kvcache_misses(const hu_llamacpp_kvcache_t *cache);

#endif /* HU_LLAMACPP_KVCACHE_H */
