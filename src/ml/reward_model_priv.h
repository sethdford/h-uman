/* src/ml/reward_model_priv.h — Private context for HUML reward model
 *
 * Internal header (not installed). Shared between reward_model.c,
 * reward_model_train.c and other training-related modules. Opaque to callers.
 */
#ifndef HU_ML_REWARD_MODEL_PRIV_H
#define HU_ML_REWARD_MODEL_PRIV_H

#include "human/core/allocator.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/value_head.h"
#include <stddef.h>

typedef struct {
    hu_allocator_t *alloc;
    hu_model_t backbone;           /* Frozen GPT backbone */
    hu_value_head_t value_head;    /* Trainable value head */
    hu_gpt_config_t gpt_cfg;       /* Config used to create backbone */
    size_t vocab_size;
    size_t hidden_dim;
} huml_rm_ctx_t;

#endif /* HU_ML_REWARD_MODEL_PRIV_H */
