#ifndef HU_MEMORY_RETRIEVAL_STRATEGY_LEARNER_H
#define HU_MEMORY_RETRIEVAL_STRATEGY_LEARNER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Strategy Learner — tracks retrieval precision per query category
 * and learns which retrieval strategy works best for which type.
 * Uses historical outcomes to route future queries optimally.
 */

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

typedef enum hu_query_category {
    HU_QCAT_FACTUAL = 0, /* "what is X", "when did Y" */
    HU_QCAT_PROCEDURAL,  /* "how to X", "steps for Y" */
    HU_QCAT_PERSONAL,    /* "my preferences", "last time I" */
    HU_QCAT_TEMPORAL,    /* "yesterday", "last week", time-based */
    HU_QCAT_SEMANTIC,    /* abstract/conceptual queries */
    HU_QCAT_EXACT,       /* specific names, IDs, code snippets */
    HU_QCAT_COUNT
} hu_query_category_t;

typedef enum hu_retrieval_strategy {
    HU_RSTRAT_KEYWORD = 0,
    HU_RSTRAT_VECTOR,
    HU_RSTRAT_HYBRID,
    HU_RSTRAT_TEMPORAL,
    HU_RSTRAT_GRAPH,
    HU_RSTRAT_COUNT
} hu_retrieval_strategy_t;

typedef struct hu_strategy_stats {
    hu_retrieval_strategy_t strategy;
    double precision;  /* running average precision */
    int32_t attempts;  /* total attempts */
    int32_t successes; /* successful retrievals */
} hu_strategy_stats_t;

typedef struct hu_strategy_learner {
    hu_allocator_t *alloc;
    sqlite3 *db;
} hu_strategy_learner_t;

hu_error_t hu_strategy_learner_create(hu_allocator_t *alloc, sqlite3 *db,
                                      hu_strategy_learner_t *out);
void hu_strategy_learner_deinit(hu_strategy_learner_t *learner);
hu_error_t hu_strategy_learner_init_tables(hu_strategy_learner_t *learner);

/* Classify a query into a category. Pure heuristic, no allocations. */
hu_query_category_t hu_strategy_classify_query(const char *query, size_t query_len);

/* Record a retrieval outcome for learning. */
hu_error_t hu_strategy_learner_record(hu_strategy_learner_t *learner, hu_query_category_t category,
                                      hu_retrieval_strategy_t strategy, bool success,
                                      int64_t now_ts);

/* Get the best strategy for a query category based on learned data.
 * Falls back to HYBRID if no data. */
hu_retrieval_strategy_t hu_strategy_learner_recommend(hu_strategy_learner_t *learner,
                                                      hu_query_category_t category);

#endif /* HU_ENABLE_SQLITE */
#endif
