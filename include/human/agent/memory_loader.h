#ifndef HU_AGENT_MEMORY_LOADER_H
#define HU_AGENT_MEMORY_LOADER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Memory loader — recall relevant memories, format as markdown for context
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_memory_loader {
    hu_memory_t *memory;
    hu_retrieval_engine_t *retrieval_engine; /* optional; when set, use hybrid retrieval */
    hu_allocator_t *alloc;
    size_t max_entries;
    size_t max_context_chars;
} hu_memory_loader_t;

hu_error_t hu_memory_loader_init(hu_memory_loader_t *loader, hu_allocator_t *alloc,
                                 hu_memory_t *memory, hu_retrieval_engine_t *retrieval_engine,
                                 size_t max_entries, size_t max_context_chars);

/* Load relevant memories for a query and format them as markdown text.
 * Returns HU_OK with *out_context=NULL, *out_context_len=0 if no memories.
 * Caller owns returned string; free with alloc. */
hu_error_t hu_memory_loader_load(hu_memory_loader_t *loader, const char *query, size_t query_len,
                                 const char *session_id, size_t session_id_len, char **out_context,
                                 size_t *out_context_len);

#endif /* HU_AGENT_MEMORY_LOADER_H */
