#ifndef SC_MEMORY_LIFECYCLE_DIAGNOSTICS_H
#define SC_MEMORY_LIFECYCLE_DIAGNOSTICS_H

#include "seaclaw/memory.h"
#include "seaclaw/memory/engines/registry.h"
#include "seaclaw/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Diagnostic report (simplified for SeaClaw)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_diagnostic_cache_stats {
    size_t count;
    uint64_t hits;
    uint64_t tokens_saved;
} sc_diagnostic_cache_stats_t;

typedef struct sc_diagnostic_report {
    const char *backend_name;
    size_t backend_name_len;
    bool backend_healthy;
    size_t entry_count;
    sc_backend_capabilities_t capabilities;
    bool vector_store_active;
    size_t vector_entry_count;   /* 0 if n/a */
    bool outbox_active;
    size_t outbox_pending;       /* 0 if n/a */
    bool cache_active;
    sc_diagnostic_cache_stats_t cache_stats;  /* valid if cache_active */
    size_t retrieval_sources;
    const char *rollout_mode;
    size_t rollout_mode_len;
    bool session_store_active;
    bool query_expansion_enabled;
    bool adaptive_retrieval_enabled;
    bool llm_reranker_enabled;
    bool summarizer_enabled;
    bool semantic_cache_active;
} sc_diagnostic_report_t;

/* Populate report from memory backend and optional components.
 * Pass NULL for optional components. */
void sc_diagnostics_diagnose(sc_memory_t *memory,
    void *vector_store,      /* NULL or sc_vector_store_t * */
    void *outbox,            /* NULL */
    void *response_cache,    /* NULL or sc_memory_cache_t * */
    const sc_backend_capabilities_t *capabilities,
    size_t retrieval_sources,
    const char *rollout_mode,
    size_t rollout_mode_len,
    sc_diagnostic_report_t *out);

/* Format report as human-readable text. Caller frees result. */
char *sc_diagnostics_format_report(sc_allocator_t *alloc,
    const sc_diagnostic_report_t *report);

#endif /* SC_MEMORY_LIFECYCLE_DIAGNOSTICS_H */
