#ifndef HU_ML_EXPERIMENT_STORE_H
#define HU_ML_EXPERIMENT_STORE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/experiment.h"
#include <stddef.h>

typedef struct hu_experiment_store hu_experiment_store_t;

hu_error_t hu_experiment_store_open(hu_allocator_t *alloc, const char *db_path,
                                    hu_experiment_store_t **out);

hu_error_t hu_experiment_store_save(hu_experiment_store_t *store,
                                    const hu_experiment_result_t *result);

hu_error_t hu_experiment_store_count(hu_experiment_store_t *store, size_t *count);

void hu_experiment_store_close(hu_experiment_store_t *store);

#endif
