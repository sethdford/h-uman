/*
 * Phase 1 (RL SOTA) — KV-cache index implementation. See header for design.
 *
 * Hash sentinel: FNV-1a never naturally produces 0 from non-empty
 * input, but we still map a stray 0 result to 1 so the empty-slot
 * sentinel stays unambiguous (a hash of 0 would otherwise collide with
 * "no slot recorded yet" and produce false misses on the next call).
 */

#include "human/providers/llamacpp_kvcache.h"

#include "human/core/error.h"

#include <stdatomic.h>
#include <string.h>

uint64_t hu_llamacpp_kvcache_fnv1a(const char *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 0x100000001b3ULL;
    }
    return h == 0 ? 1 : h;
}

hu_error_t hu_llamacpp_kvcache_init(hu_llamacpp_kvcache_t *cache) {
    if (!cache)
        return HU_ERR_INVALID_ARGUMENT;
    memset(cache, 0, sizeof(*cache));
    return HU_OK;
}

void hu_llamacpp_kvcache_reset(hu_llamacpp_kvcache_t *cache) {
    if (!cache)
        return;
    /* Phase 0.3 + Phase 2 — reset() clears every cache slot but
     * preserves the lifetime hit/miss counters and the tick. reset()
     * fires on LoRA hot-swap (llamacpp.c), and operators want to see
     * that the cache had been doing useful work even after a swap
     * invalidates it. Use hu_llamacpp_kvcache_init() for a fully
     * zeroed struct. */
    for (size_t i = 0; i < HU_LLAMACPP_KVCACHE_SLOTS; i++) {
        cache->slots[i].system_prompt_hash = 0;
        cache->slots[i].n_past_system = 0;
        cache->slots[i].last_used_tick = 0;
    }
}

void hu_llamacpp_kvcache_free(hu_llamacpp_kvcache_t *cache) {
    hu_llamacpp_kvcache_reset(cache);
}

/* Phase 2 — multi-slot LRU helpers.
 *
 * Linear scan over HU_LLAMACPP_KVCACHE_SLOTS is fine at N=4 — branch
 * prediction handles it and the per-slot data fits in a single cache
 * line. If N grows past 16 we'd want a sorted-by-hash array, but that's
 * unrequired today.
 */

static hu_llamacpp_kvcache_slot_t *find_slot_by_hash(hu_llamacpp_kvcache_t *cache, uint64_t h) {
    for (size_t i = 0; i < HU_LLAMACPP_KVCACHE_SLOTS; i++) {
        if (cache->slots[i].system_prompt_hash == h)
            return &cache->slots[i];
    }
    return NULL;
}

static hu_llamacpp_kvcache_slot_t *pick_eviction_target(hu_llamacpp_kvcache_t *cache) {
    /* Prefer an empty slot (hash == 0). Otherwise the slot with the
     * smallest last_used_tick. Tie-break by lowest index for
     * deterministic test behavior. */
    hu_llamacpp_kvcache_slot_t *target = &cache->slots[0];
    for (size_t i = 0; i < HU_LLAMACPP_KVCACHE_SLOTS; i++) {
        if (cache->slots[i].system_prompt_hash == 0)
            return &cache->slots[i];
        if (cache->slots[i].last_used_tick < target->last_used_tick)
            target = &cache->slots[i];
    }
    return target;
}

hu_error_t hu_llamacpp_kvcache_record_system(hu_llamacpp_kvcache_t *cache,
                                             const char *system_prompt, size_t system_prompt_len,
                                             int32_t n_past_system) {
    if (!cache || !system_prompt || n_past_system <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    uint64_t h = hu_llamacpp_kvcache_fnv1a(system_prompt, system_prompt_len);
    cache->tick++;
    /* Re-recording an existing entry must NOT evict another slot. The
     * caller is either re-asserting the same prefix (no-op semantically)
     * or extending its decode depth (the n_past should be at least as
     * large as before, but we trust the caller). */
    hu_llamacpp_kvcache_slot_t *existing = find_slot_by_hash(cache, h);
    if (existing) {
        existing->n_past_system = n_past_system;
        existing->last_used_tick = cache->tick;
        return HU_OK;
    }
    hu_llamacpp_kvcache_slot_t *target = pick_eviction_target(cache);
    target->system_prompt_hash = h;
    target->n_past_system = n_past_system;
    target->last_used_tick = cache->tick;
    return HU_OK;
}

hu_error_t hu_llamacpp_kvcache_lookup_system(hu_llamacpp_kvcache_t *cache,
                                             const char *system_prompt, size_t system_prompt_len,
                                             int32_t *out_n_past_system) {
    if (!cache || !system_prompt || !out_n_past_system)
        return HU_ERR_INVALID_ARGUMENT;
    /* Phase 0.3 — hit/miss accounting. Programmer errors (NULL args)
     * deliberately do NOT count toward miss telemetry; only honest
     * lookups against a usable cache participate in the hit rate. */
    uint64_t h = hu_llamacpp_kvcache_fnv1a(system_prompt, system_prompt_len);
    hu_llamacpp_kvcache_slot_t *match = find_slot_by_hash(cache, h);
    if (!match || match->system_prompt_hash == 0) {
        atomic_fetch_add_explicit(&cache->misses, 1, memory_order_relaxed);
        return HU_ERR_NOT_FOUND;
    }
    *out_n_past_system = match->n_past_system;
    cache->tick++;
    match->last_used_tick = cache->tick;
    atomic_fetch_add_explicit(&cache->hits, 1, memory_order_relaxed);
    return HU_OK;
}

uint64_t hu_llamacpp_kvcache_hits(const hu_llamacpp_kvcache_t *cache) {
    if (!cache)
        return 0;
    /* Cast away const for the atomic load — atomic_load_explicit takes
     * a non-const pointer per the C11 spec, but the load itself is
     * read-only at the hardware level. */
    return atomic_load_explicit((_Atomic uint64_t *)&cache->hits, memory_order_relaxed);
}

uint64_t hu_llamacpp_kvcache_misses(const hu_llamacpp_kvcache_t *cache) {
    if (!cache)
        return 0;
    return atomic_load_explicit((_Atomic uint64_t *)&cache->misses, memory_order_relaxed);
}

void hu_llamacpp_kvcache_record_hit_savings(hu_llamacpp_kvcache_t *cache, int32_t n_tokens) {
    /* Silent no-op for NULL / non-positive n so the chat-path call site
     * stays simple: it just passes the raw lookup result without an
     * intermediate filter. */
    if (!cache || n_tokens <= 0)
        return;
    atomic_fetch_add_explicit(&cache->tokens_would_skip, (uint64_t)n_tokens, memory_order_relaxed);
}

uint64_t hu_llamacpp_kvcache_tokens_would_skip(const hu_llamacpp_kvcache_t *cache) {
    if (!cache)
        return 0;
    return atomic_load_explicit((_Atomic uint64_t *)&cache->tokens_would_skip,
                                memory_order_relaxed);
}
