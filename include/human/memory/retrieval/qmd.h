#ifndef HU_MEMORY_RETRIEVAL_QMD_H
#define HU_MEMORY_RETRIEVAL_QMD_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>

/* QMD CLI adapter — spawns qmd for search. In HU_IS_TEST, returns empty (no spawn). */
hu_error_t hu_qmd_keyword_candidates(hu_allocator_t *alloc, const char *workspace_dir,
                                     size_t workspace_len, const char *query, size_t query_len,
                                     unsigned limit, hu_memory_entry_t **out, size_t *out_count);

#endif /* HU_MEMORY_RETRIEVAL_QMD_H */
