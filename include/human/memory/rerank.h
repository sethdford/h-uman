#ifndef HU_MEMORY_RERANK_H
#define HU_MEMORY_RERANK_H

#include "human/core/error.h"
#include <stddef.h>

typedef struct hu_search_result {
    char *content;
    /* memories.key of the row this result came from (owned, freed by
     * hu_rerank_free_results), or NULL when the producer had no key. Carried
     * through RRF merge and the in-place sorts so consumers converting back to
     * hu_memory_entry_t can restore the real key instead of copying content
     * into the key column (the 2026-09-03 C2-ablation contamination). */
    char *key;
    size_t key_len;
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

/* Score-level lexical+dense fusion (arXiv 2606.04194, training-free).
 * Each leg's scores are min-max normalised per call to [0,1] (a leg whose
 * scores are all equal gives every row 1.0; an empty leg contributes
 * nothing), then fused = alpha*dense + (1-alpha)*lexical, a row missing from
 * a leg scoring 0 for it. BOTH legs must arrive higher-is-better: the
 * keyword leg converts SQLite's lower-is-better bm25() with
 * hu_rerank_bm25_to_relevance before calling this. Rows are deduplicated by
 * content (the rule hu_rerank_rrf uses; a row seen twice in one leg keeps its
 * best score), sorted by fused score desc, ties broken by the RRF(k=60) score
 * and then by first appearance (keyword leg first), and only then cut to
 * max_results -- so a strong dense-only row is never dropped by the cap the
 * way insertion order would drop it. rerank_score = the fused score; score,
 * key, original_rank and candidate_idx come from the row's first appearance.
 * alpha outside [0,1] or NaN -> HU_ERR_INVALID_ARGUMENT. */
hu_error_t hu_rerank_score_fusion(hu_search_result_t *keyword_results, size_t keyword_count,
                                  hu_search_result_t *vector_results, size_t vector_count,
                                  hu_search_result_t *merged_out, size_t max_results,
                                  size_t *merged_count, float alpha);

/* Convert a backend recall list's engine scores (SQLite FTS5 bm25(): negative,
 * lower is better) into higher-is-better lexical relevance, in recall order.
 * boosts (nullable) is what the engine ADDED to each score (its graph rerank:
 * a positive "this row is better" nudge that, added to a lower-is-better
 * bm25, reads as a penalty). The boost is taken out before negating and added
 * back after: base[i] = -(engine_scores[i] - boosts[i]), clamped so it never
 * exceeds base[i-1], then out[i] = base[i] + boosts[i]. The clamp keeps rows
 * the engine appended after its FTS hits (spreading activation, hierarchy
 * members: positive scores, no boost) from outranking a row above them; a
 * boost on an FTS row lifts it and is never flattened by the clamp. A
 * non-finite score (the LIKE fallback) ties with the row above; a leading one
 * is 0. out may alias engine_scores. */
void hu_rerank_bm25_to_relevance(const double *engine_scores, const double *boosts, size_t count,
                                 double *out);

/* HU_HYBRID_FUSION=rrf|score picks the plain hybrid merge (default rrf; unset
 * or empty -> rrf; anything else -> rrf with a one-shot warning naming the
 * variable). HU_HYBRID_FUSION_ALPHA is the dense weight for score fusion,
 * a float in [0,1] (default HU_HYBRID_FUSION_ALPHA_DEFAULT; invalid -> the
 * default with a one-shot warning). Both read the environment on every call. */
typedef enum hu_hybrid_fusion {
    HU_HYBRID_FUSION_RRF = 0,
    HU_HYBRID_FUSION_SCORE = 1,
} hu_hybrid_fusion_t;

#define HU_HYBRID_FUSION_ALPHA_DEFAULT 0.5f

hu_hybrid_fusion_t hu_hybrid_fusion_mode(void);
float hu_hybrid_fusion_alpha(void);

/* Cross-encoder rerank (simple: score by query-document term overlap).
 * rerank_score = matching_query_words / total_query_words. Re-sorts in place. */
hu_error_t hu_rerank_cross_encoder(const char *query, hu_search_result_t *results, size_t count);

/* Free content of results (caller owns the array). */
void hu_rerank_free_results(hu_search_result_t *results, size_t count);

#endif /* HU_MEMORY_RERANK_H */
