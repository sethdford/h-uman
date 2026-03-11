#ifndef HU_INTELLIGENCE_META_LEARNING_H
#define HU_INTELLIGENCE_META_LEARNING_H

#include "human/core/error.h"
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

typedef struct hu_meta_params {
    double default_confidence_threshold;
    int refinement_frequency_weeks;
    int discovery_min_feedback_count;
} hu_meta_params_t;

hu_error_t hu_meta_learning_load(sqlite3 *db, hu_meta_params_t *out);
hu_error_t hu_meta_learning_update(sqlite3 *db, const hu_meta_params_t *params);
hu_error_t hu_meta_learning_optimize(sqlite3 *db, hu_meta_params_t *out);
#endif

#endif /* HU_INTELLIGENCE_META_LEARNING_H */
