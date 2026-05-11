#ifndef HU_AGENT_MEMORY_LOADER_H
#define HU_AGENT_MEMORY_LOADER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include <stddef.h>

/* Forward-declare the W7 facade handle to avoid the legacy hu_memory_t /
 * hu_memory_facade_t type collision. See world_model_bridge.h for details. */
struct hu_w7_facade;
struct hu_personal_model;

/* ──────────────────────────────────────────────────────────────────────────
 * Memory loader — recall relevant memories, format as markdown for context
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_memory_loader {
    hu_memory_t *memory;
    hu_retrieval_engine_t *retrieval_engine; /* optional; when set, use hybrid retrieval */
    hu_allocator_t *alloc;
    size_t max_entries;
    size_t max_context_chars;
    struct hu_w7_facade *facade; /* optional W7 facade for W12 planner recall */
    struct hu_personal_model *personal_model; /* optional; merged into W9 graph context */
} hu_memory_loader_t;

hu_error_t hu_memory_loader_init(hu_memory_loader_t *loader, hu_allocator_t *alloc,
                                 hu_memory_t *memory, hu_retrieval_engine_t *retrieval_engine,
                                 size_t max_entries, size_t max_context_chars);

/* Bind the W7 facade for goal-conditioned planner recall (`hu_w12_planner_recall`).
 * Call after `hu_memory_loader_init` when `agent->w7_facade` is non-NULL; safe to
 * pass NULL to clear. Do not mutate `loader->facade` directly — keeps Phase-1 wiring
 * auditable and grep-friendly. */
void hu_memory_loader_set_facade(hu_memory_loader_t *loader, struct hu_w7_facade *facade);

/* Bind the personal model for W9 graph context enrichment. When set, the
 * supplementary world-model render merges personal style/topics/goals into
 * the graph snapshot. Safe to pass NULL. */
void hu_memory_loader_set_personal_model(hu_memory_loader_t *loader,
                                         struct hu_personal_model *pm);

/* Load relevant memories for a query and format them as markdown text.
 * Returns HU_OK with *out_context=NULL, *out_context_len=0 if no memories.
 * Caller owns returned string; free with alloc. */
hu_error_t hu_memory_loader_load(hu_memory_loader_t *loader, const char *query, size_t query_len,
                                 const char *session_id, size_t session_id_len, char **out_context,
                                 size_t *out_context_len);

#endif /* HU_AGENT_MEMORY_LOADER_H */
