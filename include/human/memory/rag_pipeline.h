#ifndef HU_MEMORY_RAG_PIPELINE_H
#define HU_MEMORY_RAG_PIPELINE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* F153-F154 — Unified RAG Pipeline */

typedef enum hu_rag_source {
    HU_RAG_EPISODIC = 0,
    HU_RAG_SUPERHUMAN,
    HU_RAG_KNOWLEDGE,
    HU_RAG_COMPRESSION,
    HU_RAG_OPINIONS,
    HU_RAG_CHAPTERS,
    HU_RAG_SOCIAL_GRAPH,
    HU_RAG_FEEDS,
    HU_RAG_SOURCE_COUNT
} hu_rag_source_t;

typedef struct hu_rag_result {
    hu_rag_source_t source;
    char *content;
    size_t content_len;
    double relevance;
    double freshness;
    double combined_score;
} hu_rag_result_t;

typedef struct hu_rag_config {
    double source_weights[HU_RAG_SOURCE_COUNT]; /* default all 1.0 */
    uint32_t max_results;                       /* default 10 */
    double min_relevance;                       /* default 0.3 */
    size_t max_token_budget;                    /* default 2000 */
} hu_rag_config_t;

hu_rag_config_t hu_rag_default_config(void);
double hu_rag_compute_score(double relevance, double freshness, double source_weight);
void hu_rag_sort_results(hu_rag_result_t *results, size_t count);
size_t hu_rag_select_within_budget(hu_rag_result_t *results, size_t count, size_t max_tokens);
hu_error_t hu_rag_build_prompt(hu_allocator_t *alloc, const hu_rag_result_t *results,
                               size_t count, char **out, size_t *out_len);
const char *hu_rag_source_str(hu_rag_source_t source);
void hu_rag_result_deinit(hu_allocator_t *alloc, hu_rag_result_t *r);

/* F155-F156 — On-Device Classification */

typedef enum hu_classify_result {
    HU_CLASS_GREETING = 0,
    HU_CLASS_QUESTION,
    HU_CLASS_EMOTIONAL,
    HU_CLASS_INFORMATIONAL,
    HU_CLASS_PLANNING,
    HU_CLASS_HUMOR,
    HU_CLASS_URGENT,
    HU_CLASS_CASUAL,
    HU_CLASS_UNKNOWN
} hu_classify_result_t;

typedef struct hu_classifier_config {
    bool use_keywords;           /* fallback keyword-based */
    double confidence_threshold; /* default 0.6 */
} hu_classifier_config_t;

hu_classify_result_t hu_classify_message(const char *message, size_t msg_len,
                                         double *confidence_out);
const char *hu_classify_result_str(hu_classify_result_t r);
hu_error_t hu_classifier_build_prompt(hu_allocator_t *alloc, hu_classify_result_t cls,
                                      double confidence, char **out, size_t *out_len);

#endif /* HU_MEMORY_RAG_PIPELINE_H */
