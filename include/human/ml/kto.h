/* include/human/ml/kto.h
 *
 * Backend factory declarations for KTO (Kahneman-Tversky Optimization)
 * trainers. Dispatched by hu_rl_trainer_create_kto in src/ml/rl_trainer.c.
 *
 * HUML: in-process toy GPT, cross-platform, gradient-checkable.
 * MLX:  Apple-only subprocess wrapping mlx-lm-lora KTO trainer.
 */
#ifndef HU_ML_KTO_H
#define HU_ML_KTO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/rl_trainer.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_kto_huml_create(hu_allocator_t *alloc,
                               const hu_rl_trainer_config_t *config,
                               hu_rl_trainer_t *out);

/* Returns HU_ERR_NOT_SUPPORTED until Task 7 fills in the MLX path. */
hu_error_t hu_kto_mlx_create(hu_allocator_t *alloc,
                              const hu_rl_trainer_config_t *config,
                              hu_rl_trainer_t *out);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_KTO_H */
