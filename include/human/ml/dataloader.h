#ifndef HU_ML_DATALOADER_H
#define HU_ML_DATALOADER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Data loading interface
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_ml_batch {
    int32_t *input_ids;
    int32_t *target_ids;
    size_t batch_size;
    size_t seq_len;
    int epoch;
} hu_ml_batch_t;

typedef struct hu_ml_dataloader hu_ml_dataloader_t;

hu_error_t hu_ml_dataloader_create(hu_allocator_t *alloc, const char *data_dir,
                                   size_t batch_size, size_t seq_len,
                                   const char *split,
                                   hu_ml_dataloader_t **out);

hu_error_t hu_ml_dataloader_next(hu_ml_dataloader_t *dl, hu_ml_batch_t *batch);

void hu_ml_dataloader_reset(hu_ml_dataloader_t *dl);

void hu_ml_dataloader_deinit(hu_ml_dataloader_t *dl);

void hu_ml_batch_free(hu_allocator_t *alloc, hu_ml_batch_t *batch);

#endif
