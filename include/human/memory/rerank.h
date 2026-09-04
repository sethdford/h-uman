#ifndef HU_MEMORY_RERANK_H
#define HU_MEMORY_RERANK_H

#include "human/core/error.h"
#include <stddef.h>

typedef struct hu_search_result {
    char *content;
    float score;        /* original score (BM25 or cosine) */
    float rerank_score; /* after reranking */
    size_t original_rank;
    /* Contract C2: opaque index the caller may stash before reranking. Untouched
     * by hu_rerank_rrf/hu_rerank_cross_encoder except that their in-place sorts
     * swap the whole struct, so this value travels with its row and lets a caller
     * map the reranked order back to metadata (e.g. session_id/timestamp) that
     * this struct doesn't carry. Defaults to 0 for every existing caller. */
    size_t candidate_idx;
} hu_search_result_t;

/* Reciprocal Rank Fusion — merge keyword + vector results.
 * rrf_score = sum(1.0 / (k + rank)) across lists. k default 60.0. */
hu_error_t hu_rerank_rrf(hu_search_result_t *keyword_results, size_t keyword_count,
                         hu_search_result_t *vector_results, size_t vector_count,
                         hu_search_result_t *merged_out, size_t max_results, size_t *merged_count,
                         float k);

/* Cross-encoder rerank (simple: score by query-document term overlap).
 * rerank_score = matching_query_words / total_query_words. Re-sorts in place. */
hu_error_t hu_rerank_cross_encoder(const char *query, hu_search_result_t *results, size_t count);

/* Free content of results (caller owns the array). */
void hu_rerank_free_results(hu_search_result_t *results, size_t count);

#endif /* HU_MEMORY_RERANK_H */
