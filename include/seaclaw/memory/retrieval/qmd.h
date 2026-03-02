#ifndef SC_MEMORY_RETRIEVAL_QMD_H
#define SC_MEMORY_RETRIEVAL_QMD_H

#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* QMD CLI adapter — spawns qmd for search. In SC_IS_TEST, returns empty (no spawn). */
sc_error_t sc_qmd_keyword_candidates(sc_allocator_t *alloc,
    const char *workspace_dir, size_t workspace_len,
    const char *query, size_t query_len,
    unsigned limit,
    sc_memory_entry_t **out, size_t *out_count);

#endif /* SC_MEMORY_RETRIEVAL_QMD_H */
