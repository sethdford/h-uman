#ifndef HU_ML_EXPERIMENT_H
#define HU_ML_EXPERIMENT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/ml.h"
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Experiment loop API
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_experiment_status {
    HU_EXPERIMENT_KEEP,
    HU_EXPERIMENT_DISCARD,
    HU_EXPERIMENT_CRASH,
} hu_experiment_status_t;

typedef struct hu_experiment_result {
    hu_experiment_config_t config;
    double val_bpb;
    double peak_memory_mb;
    double training_seconds;
    hu_experiment_status_t status;
    char description[256];
    int iteration;
} hu_experiment_result_t;

typedef struct hu_experiment_loop_config {
    int max_iterations;
    hu_experiment_config_t base_config;
    const char *data_dir;
    const char *results_path;
    double convergence_threshold;
} hu_experiment_loop_config_t;

typedef void (*hu_experiment_loop_callback_t)(const hu_experiment_result_t *result,
                                              void *user_data);

hu_error_t hu_experiment_loop(hu_allocator_t *alloc,
                              const hu_experiment_loop_config_t *config,
                              hu_experiment_loop_callback_t callback,
                              void *user_data);

hu_error_t hu_experiment_result_to_tsv(const hu_experiment_result_t *result,
                                       char *buf, size_t buf_size);

#endif
