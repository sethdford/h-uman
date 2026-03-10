#ifndef HU_MEMORY_LIFECYCLE_DIAGNOSTICS_H
#define HU_MEMORY_LIFECYCLE_DIAGNOSTICS_H

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/engines/registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Diagnostic report (simplified for Human)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_diagnostic_cache_stats {
    size_t count;
    uint64_t hits;
    uint64_t tokens_saved;
} hu_diagnostic_cache_stats_t;

typedef struct hu_diagnostic_report {
    const char *backend_name;
    size_t backend_name_len;
    bool backend_healthy;
    size_t entry_count;
    hu_backend_capabilities_t capabilities;
    bool vector_store_active;
    size_t vector_entry_count; /* 0 if n/a */
    bool outbox_active;
    size_t outbox_pending; /* 0 if n/a */
    bool cache_active;
    hu_diagnostic_cache_stats_t cache_stats; /* valid if cache_active */
    size_t retrieval_sources;
    const char *rollout_mode;
    size_t rollout_mode_len;
    bool session_store_active;
    bool query_expansion_enabled;
    bool adaptive_retrieval_enabled;
    bool llm_reranker_enabled;
    bool summarizer_enabled;
    bool semantic_cache_active;
} hu_diagnostic_report_t;

/* Populate report from memory backend and optional components.
 * Pass NULL for optional components. */
void hu_diagnostics_diagnose(hu_memory_t *memory,
                             void *vector_store,   /* NULL or hu_vector_store_t * */
                             void *outbox,         /* NULL */
                             void *response_cache, /* NULL or hu_memory_cache_t * */
                             const hu_backend_capabilities_t *capabilities,
                             size_t retrieval_sources, const char *rollout_mode,
                             size_t rollout_mode_len, hu_diagnostic_report_t *out);

/* Format report as human-readable text. Caller frees result. */
char *hu_diagnostics_format_report(hu_allocator_t *alloc, const hu_diagnostic_report_t *report);

#endif /* HU_MEMORY_LIFECYCLE_DIAGNOSTICS_H */
