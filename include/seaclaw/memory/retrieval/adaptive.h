#ifndef SC_MEMORY_RETRIEVAL_ADAPTIVE_H
#define SC_MEMORY_RETRIEVAL_ADAPTIVE_H

#include <stdbool.h>
#include <stddef.h>

/* Recommended retrieval strategy based on query analysis */
typedef enum sc_adaptive_strategy {
    SC_ADAPTIVE_KEYWORD_ONLY,
    SC_ADAPTIVE_VECTOR_ONLY,
    SC_ADAPTIVE_HYBRID,
} sc_adaptive_strategy_t;

typedef struct sc_adaptive_config {
    bool enabled;
    unsigned keyword_max_tokens;  /* <= this -> keyword_only */
    unsigned vector_min_tokens;  /* >= this -> vector_only or hybrid */
} sc_adaptive_config_t;

typedef struct sc_query_analysis {
    unsigned token_count;
    bool has_special_chars;
    bool is_question;
    float avg_token_length;
    sc_adaptive_strategy_t recommended_strategy;
} sc_query_analysis_t;

/* Analyze query and recommend strategy. Pure function, no allocations. */
sc_query_analysis_t sc_adaptive_analyze_query(const char *query, size_t query_len,
    const sc_adaptive_config_t *config);

#endif /* SC_MEMORY_RETRIEVAL_ADAPTIVE_H */
