#ifndef HU_ML_CHECKPOINT_H
#define HU_ML_CHECKPOINT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"

hu_error_t hu_ml_checkpoint_save(hu_allocator_t *alloc, const char *path,
                                 hu_model_t *model, hu_ml_optimizer_t *opt);

hu_error_t hu_ml_checkpoint_load(hu_allocator_t *alloc, const char *path,
                                 hu_model_t *model, hu_ml_optimizer_t *opt);

#endif
