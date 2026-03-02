#ifndef SC_AGENT_MEMORY_LOADER_H
#define SC_AGENT_MEMORY_LOADER_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Memory loader — recall relevant memories, format as markdown for context
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_memory_loader {
    sc_memory_t *memory;
    sc_allocator_t *alloc;
    size_t max_entries;
    size_t max_context_chars;
} sc_memory_loader_t;

sc_error_t sc_memory_loader_init(sc_memory_loader_t *loader,
    sc_allocator_t *alloc, sc_memory_t *memory,
    size_t max_entries, size_t max_context_chars);

/* Load relevant memories for a query and format them as markdown text.
 * Returns SC_OK with *out_context=NULL, *out_context_len=0 if no memories.
 * Caller owns returned string; free with alloc. */
sc_error_t sc_memory_loader_load(sc_memory_loader_t *loader,
    const char *query, size_t query_len,
    const char *session_id, size_t session_id_len,
    char **out_context, size_t *out_context_len);

#endif /* SC_AGENT_MEMORY_LOADER_H */
