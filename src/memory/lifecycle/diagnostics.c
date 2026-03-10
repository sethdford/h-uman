#include "human/memory/lifecycle/diagnostics.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory/lifecycle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hu_diagnostics_diagnose(hu_memory_t *memory, void *vector_store, void *outbox,
                             void *response_cache, const hu_backend_capabilities_t *capabilities,
                             size_t retrieval_sources, const char *rollout_mode,
                             size_t rollout_mode_len, hu_diagnostic_report_t *out) {
    memset(out, 0, sizeof(*out));
    if (!memory || !out)
        return;

    out->backend_name = memory->vtable->name(memory->ctx);
    out->backend_name_len = out->backend_name ? strlen(out->backend_name) : 0;
    out->backend_healthy = memory->vtable->health_check(memory->ctx);

    size_t count = 0;
    if (memory->vtable->count(memory->ctx, &count) == HU_OK)
        out->entry_count = count;

    if (capabilities)
        out->capabilities = *capabilities;

    out->vector_store_active = (vector_store != NULL);
    out->outbox_active = (outbox != NULL);
    out->cache_active = (response_cache != NULL);
    out->retrieval_sources = retrieval_sources;
    out->rollout_mode = rollout_mode ? rollout_mode : "off";
    out->rollout_mode_len = rollout_mode_len ? rollout_mode_len : 3;
    out->session_store_active = false; /* Human doesn't expose session store in this path */

    if (response_cache) {
        out->cache_stats.count = hu_memory_cache_count((hu_memory_cache_t *)response_cache);
        /* hits and tokens_saved would need cache API extension */
    }
}

char *hu_diagnostics_format_report(hu_allocator_t *alloc, const hu_diagnostic_report_t *report) {
    if (!alloc || !report)
        return NULL;

    /* Estimate size: header + sections ~1.5K */
    size_t cap = 2048;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;

    int n = 0;
    n += snprintf(buf + n, cap - (size_t)n,
                  "=== Memory Doctor ===\n\n"
                  "Backend\n"
                  "  name:    %.*s\n"
                  "  healthy: %s\n"
                  "  entries: %zu\n\n"
                  "Capabilities\n"
                  "  keyword_rank:  %s\n"
                  "  session_store: %s\n"
                  "  transactions:  %s\n"
                  "  outbox:        %s\n\n"
                  "Vector Plane\n"
                  "  active:  %s\n"
                  "  vectors: %s\n\n"
                  "Outbox\n"
                  "  active:  %s\n"
                  "  pending: %s\n\n"
                  "Response Cache\n"
                  "  active: %s\n"
                  "  count:  %zu\n\n"
                  "Retrieval\n"
                  "  sources: %d\n"
                  "  rollout: %.*s\n\n"
                  "Pipeline Stages\n"
                  "  query_expansion: %s\n"
                  "  adaptive:        %s\n"
                  "  llm_reranker:    %s\n"
                  "  summarizer:      %s\n"
                  "  semantic_cache:  %s\n",
                  (int)report->backend_name_len, report->backend_name ? report->backend_name : "",
                  report->backend_healthy ? "true" : "false", report->entry_count,
                  report->capabilities.supports_keyword_rank ? "true" : "false",
                  report->capabilities.supports_session_store ? "true" : "false",
                  report->capabilities.supports_transactions ? "true" : "false",
                  report->capabilities.supports_outbox ? "true" : "false",
                  report->vector_store_active ? "true" : "false",
                  report->vector_store_active ? (report->vector_entry_count ? "n/a" : "0") : "n/a",
                  report->outbox_active ? "true" : "false", report->outbox_active ? "n/a" : "n/a",
                  report->cache_active ? "true" : "false", report->cache_stats.count,
                  (int)report->retrieval_sources, (int)report->rollout_mode_len,
                  report->rollout_mode ? report->rollout_mode : "off",
                  report->query_expansion_enabled ? "true" : "false",
                  report->adaptive_retrieval_enabled ? "true" : "false",
                  report->llm_reranker_enabled ? "true" : "false",
                  report->summarizer_enabled ? "true" : "false",
                  report->semantic_cache_active ? "true" : "false");

    (void)n;
    return buf;
}
