#ifndef HU_MEMORY_RETRIEVAL_LLM_RERANKER_H
#define HU_MEMORY_RETRIEVAL_LLM_RERANKER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>

/* Build rerank prompt for LLM. Caller frees result. */
char *hu_llm_reranker_build_prompt(hu_allocator_t *alloc, const char *query, size_t query_len,
                                   const hu_memory_entry_t *entries, size_t count,
                                   unsigned max_candidates);

/* Parse LLM response to get 1-based indices. Returns count, or 0 on parse failure.
 * Indices are written to out_indices (caller allocates, size limit). */
size_t hu_llm_reranker_parse_response(const char *response, size_t response_len,
                                      size_t *out_indices, size_t max_indices);

#endif /* HU_MEMORY_RETRIEVAL_LLM_RERANKER_H */
