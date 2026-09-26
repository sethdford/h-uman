#ifndef HU_MEMORY_LIFECYCLE_H
#define HU_MEMORY_LIFECYCLE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Cache: in-memory LRU cache for memory entries
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_memory_cache hu_memory_cache_t;

hu_memory_cache_t *hu_memory_cache_create(hu_allocator_t *alloc, size_t max_entries);
void hu_memory_cache_destroy(hu_memory_cache_t *cache);
hu_error_t hu_memory_cache_get(hu_memory_cache_t *cache, const char *key, size_t key_len,
                               hu_memory_entry_t *out, bool *found);
hu_error_t hu_memory_cache_put(hu_memory_cache_t *cache, const char *key, size_t key_len,
                               const hu_memory_entry_t *entry);
void hu_memory_cache_invalidate(hu_memory_cache_t *cache, const char *key, size_t key_len);
void hu_memory_cache_clear(hu_memory_cache_t *cache);
size_t hu_memory_cache_count(const hu_memory_cache_t *cache);

#endif /* HU_MEMORY_LIFECYCLE_H */
