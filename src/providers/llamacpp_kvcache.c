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
    /* Phase 0.3 — reset() clears the cache SLOT but preserves the
     * lifetime hit/miss counters. reset() fires on LoRA hot-swap
     * (llamacpp.c:455), and operators want to see the cache had been
     * doing useful work even after a swap invalidates it. Use
     * hu_llamacpp_kvcache_init() if you want a fully zeroed struct. */
    cache->system_prompt_hash = 0;
    cache->n_past_system = 0;
}

void hu_llamacpp_kvcache_free(hu_llamacpp_kvcache_t *cache) {
    hu_llamacpp_kvcache_reset(cache);
}

hu_error_t hu_llamacpp_kvcache_record_system(hu_llamacpp_kvcache_t *cache,
                                             const char *system_prompt, size_t system_prompt_len,
                                             int32_t n_past_system) {
    if (!cache || !system_prompt || n_past_system <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    cache->system_prompt_hash = hu_llamacpp_kvcache_fnv1a(system_prompt, system_prompt_len);
    cache->n_past_system = n_past_system;
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
    if (cache->system_prompt_hash == 0) {
        atomic_fetch_add_explicit(&cache->misses, 1, memory_order_relaxed);
        return HU_ERR_NOT_FOUND;
    }
    uint64_t h = hu_llamacpp_kvcache_fnv1a(system_prompt, system_prompt_len);
    if (h != cache->system_prompt_hash) {
        atomic_fetch_add_explicit(&cache->misses, 1, memory_order_relaxed);
        return HU_ERR_NOT_FOUND;
    }
    *out_n_past_system = cache->n_past_system;
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
