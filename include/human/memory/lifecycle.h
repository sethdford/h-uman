#ifndef HU_MEMORY_LIFECYCLE_H
#define HU_MEMORY_LIFECYCLE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/provider.h"
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

/* ──────────────────────────────────────────────────────────────────────────
 * Hygiene: cleanup stale/duplicate/oversized memories
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_hygiene_config {
    size_t max_entries;
    size_t max_entry_size;
    uint64_t max_age_seconds;
    bool deduplicate;
} hu_hygiene_config_t;

typedef struct hu_hygiene_stats {
    size_t entries_scanned;
    size_t entries_removed;
    size_t duplicates_removed;
    size_t oversized_removed;
    size_t expired_removed;
} hu_hygiene_stats_t;

hu_error_t hu_memory_hygiene_run(hu_allocator_t *alloc, hu_memory_t *memory,
                                 const hu_hygiene_config_t *config, hu_hygiene_stats_t *stats);

/* ──────────────────────────────────────────────────────────────────────────
 * Snapshot: export/import memory state
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_memory_snapshot_export(hu_allocator_t *alloc, hu_memory_t *memory, const char *path,
                                     size_t path_len);
hu_error_t hu_memory_snapshot_import(hu_allocator_t *alloc, hu_memory_t *memory, const char *path,
                                     size_t path_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Summarizer: compress old memories
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_summarizer_config {
    size_t batch_size;
    size_t max_summary_len;
    hu_provider_t *provider; /* LLM provider for summarization, NULL for simple truncation */
} hu_summarizer_config_t;

typedef struct hu_summarizer_stats {
    size_t entries_summarized;
    size_t tokens_saved;
} hu_summarizer_stats_t;

hu_error_t hu_memory_summarize(hu_allocator_t *alloc, hu_memory_t *memory,
                               const hu_summarizer_config_t *config, hu_summarizer_stats_t *stats);

#endif /* HU_MEMORY_LIFECYCLE_H */
