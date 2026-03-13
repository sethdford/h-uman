#ifndef HU_ML_TRAIN_H
#define HU_ML_TRAIN_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dataloader.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Training loop API
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_ml_train(hu_allocator_t *alloc, hu_model_t *model,
                       hu_ml_optimizer_t *optimizer,
                       hu_ml_dataloader_t *train_loader,
                       hu_ml_dataloader_t *val_loader,
                       const hu_training_config_t *config,
                       const int32_t *token_bytes, size_t vocab_size,
                       hu_ml_train_result_t *result);

#endif
