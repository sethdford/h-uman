#ifndef HU_MEMORY_RETRIEVAL_ADAPTIVE_H
#define HU_MEMORY_RETRIEVAL_ADAPTIVE_H

#include <stdbool.h>
#include <stddef.h>

/* Recommended retrieval strategy based on query analysis */
typedef enum hu_adaptive_strategy {
    HU_ADAPTIVE_KEYWORD_ONLY,
    HU_ADAPTIVE_VECTOR_ONLY,
    HU_ADAPTIVE_HYBRID,
} hu_adaptive_strategy_t;

typedef struct hu_adaptive_config {
    bool enabled;
    unsigned keyword_max_tokens; /* <= this -> keyword_only */
    unsigned vector_min_tokens;  /* >= this -> vector_only or hybrid */
} hu_adaptive_config_t;

typedef struct hu_query_analysis {
    unsigned token_count;
    bool has_special_chars;
    bool is_question;
    float avg_token_length;
    hu_adaptive_strategy_t recommended_strategy;
} hu_query_analysis_t;

/* Analyze query and recommend strategy. Pure function, no allocations. */
hu_query_analysis_t hu_adaptive_analyze_query(const char *query, size_t query_len,
                                              const hu_adaptive_config_t *config);

#endif /* HU_MEMORY_RETRIEVAL_ADAPTIVE_H */
