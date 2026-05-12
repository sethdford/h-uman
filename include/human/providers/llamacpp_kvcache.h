#ifndef HU_LLAMACPP_KVCACHE_H
#define HU_LLAMACPP_KVCACHE_H

/*
 * Phase 1 (RL SOTA) — KV-cache index for the in-process llama.cpp
 * provider.
 *
 * The actual KV cache lives inside llama_context (managed by upstream).
 * What this module owns is the bookkeeping that lets chat_with_system
 * decide whether the last call's system-prefix tokens are still resident
 * in the llama_context's KV cache and can be reused on the next call.
 *
 * Design (Phase 1 simplification):
 *   - Track ONE slot: the most-recent system-prompt hash + the n_past
 *     index after decoding that prefix.
 *   - On a hit (hash matches), the chat path skips re-decoding the
 *     system prefix and resumes from n_past_system.
 *   - On a miss, the chat path calls llama_kv_self_clear() on the
 *     llama_context, decodes the new prefix, and records the new
 *     hash + n_past here.
 *   - Adapter load/unload (LoRA hot-swap) MUST call
 *     hu_llamacpp_kvcache_reset() because per-token KV depends on the
 *     model's effective weights.
 *
 * Multi-prefix / serialized-state KV caching is a Phase 3+ optimization
 * per the umbrella plan; this Phase 1 module gets us system-prompt
 * reuse with no llama-state-serialization complexity.
 */

#include "human/core/error.h"

#include <stddef.h>
#include <stdint.h>

typedef struct hu_llamacpp_kvcache {
    uint64_t system_prompt_hash; /* 0 -> empty slot */
    int32_t  n_past_system;      /* token count consumed by the prefix */
} hu_llamacpp_kvcache_t;

hu_error_t hu_llamacpp_kvcache_init(hu_llamacpp_kvcache_t *cache);

/* Record that `system_prompt` was decoded into the llama_context and the
 * cursor is now at `n_past_system`. Overwrites any prior slot. */
hu_error_t hu_llamacpp_kvcache_record_system(hu_llamacpp_kvcache_t *cache,
                                             const char *system_prompt,
                                             size_t system_prompt_len,
                                             int32_t n_past_system);

/* Look up `system_prompt`. On hit: HU_OK + *out_n_past_system set.
 * On miss: HU_ERR_NOT_FOUND, *out_n_past_system left untouched. */
hu_error_t hu_llamacpp_kvcache_lookup_system(hu_llamacpp_kvcache_t *cache,
                                             const char *system_prompt,
                                             size_t system_prompt_len,
                                             int32_t *out_n_past_system);

void hu_llamacpp_kvcache_reset(hu_llamacpp_kvcache_t *cache);
void hu_llamacpp_kvcache_free(hu_llamacpp_kvcache_t *cache);

uint64_t hu_llamacpp_kvcache_fnv1a(const char *data, size_t len);

#endif /* HU_LLAMACPP_KVCACHE_H */
