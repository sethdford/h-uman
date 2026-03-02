#ifndef SC_MEMORY_LIFECYCLE_H
#define SC_MEMORY_LIFECYCLE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Cache: in-memory LRU cache for memory entries
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_memory_cache sc_memory_cache_t;

sc_memory_cache_t *sc_memory_cache_create(sc_allocator_t *alloc, size_t max_entries);
void sc_memory_cache_destroy(sc_memory_cache_t *cache);
sc_error_t sc_memory_cache_get(sc_memory_cache_t *cache, const char *key, size_t key_len,
    sc_memory_entry_t *out, bool *found);
sc_error_t sc_memory_cache_put(sc_memory_cache_t *cache, const char *key, size_t key_len,
    const sc_memory_entry_t *entry);
void sc_memory_cache_invalidate(sc_memory_cache_t *cache, const char *key, size_t key_len);
void sc_memory_cache_clear(sc_memory_cache_t *cache);
size_t sc_memory_cache_count(const sc_memory_cache_t *cache);

/* ──────────────────────────────────────────────────────────────────────────
 * Hygiene: cleanup stale/duplicate/oversized memories
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_hygiene_config {
    size_t max_entries;
    size_t max_entry_size;
    uint64_t max_age_seconds;
    bool deduplicate;
} sc_hygiene_config_t;

typedef struct sc_hygiene_stats {
    size_t entries_scanned;
    size_t entries_removed;
    size_t duplicates_removed;
    size_t oversized_removed;
    size_t expired_removed;
} sc_hygiene_stats_t;

sc_error_t sc_memory_hygiene_run(sc_allocator_t *alloc, sc_memory_t *memory,
    const sc_hygiene_config_t *config, sc_hygiene_stats_t *stats);

/* ──────────────────────────────────────────────────────────────────────────
 * Snapshot: export/import memory state
 * ────────────────────────────────────────────────────────────────────────── */

sc_error_t sc_memory_snapshot_export(sc_allocator_t *alloc, sc_memory_t *memory,
    const char *path, size_t path_len);
sc_error_t sc_memory_snapshot_import(sc_allocator_t *alloc, sc_memory_t *memory,
    const char *path, size_t path_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Summarizer: compress old memories
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_summarizer_config {
    size_t batch_size;
    size_t max_summary_len;
    sc_provider_t *provider;  /* LLM provider for summarization, NULL for simple truncation */
} sc_summarizer_config_t;

typedef struct sc_summarizer_stats {
    size_t entries_summarized;
    size_t tokens_saved;
} sc_summarizer_stats_t;

sc_error_t sc_memory_summarize(sc_allocator_t *alloc, sc_memory_t *memory,
    const sc_summarizer_config_t *config, sc_summarizer_stats_t *stats);

#endif /* SC_MEMORY_LIFECYCLE_H */
