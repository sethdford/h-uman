/* include/human/ml/grpo.h — Phase 4 Task 0 (RL SOTA)
 *
 * Backend factory declarations for GRPO (Group Relative Policy
 * Optimization, Shao et al. 2024 — DeepSeekMath §4.1.2).  Dispatched
 * by hu_rl_trainer_create_grpo in src/ml/rl_trainer.c.
 *
 * HUML: in-process toy GPT, cross-platform, gradient-checkable.
 *       Strong impl lands at Phase 4 Task 5 in src/ml/grpo.c, which
 *       guards itself with `#define HU_GRPO_HAVE_HUML_IMPL 1` so the
 *       temporary stub in src/ml/rl_trainer.c falls out of the link.
 * MLX:  Apple-only subprocess wrapping scripts/grpo_mlx_train.py.
 *       Strong impl lands at Phase 4 Task 8 in src/ml/grpo_mlx.c with
 *       `#define HU_GRPO_HAVE_MLX_IMPL 1`.
 *
 * Precondition contract for config->kl_beta:
 *   kl_beta <  0  → use literature default 0.04 (DeepSeek R1).
 *   kl_beta == 0  → KL penalty DISABLED (R4 escape valve).
 *   kl_beta >  0  → use literal value.
 * See include/human/ml/rl_trainer.h for the canonical comment.
 */
#ifndef HU_ML_GRPO_H
#define HU_ML_GRPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/reward_source.h"
#include "human/ml/rl_trainer.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_grpo_huml_create(hu_allocator_t *alloc,
                                const hu_rl_trainer_config_t *config,
                                hu_rl_trainer_t *out);

hu_error_t hu_grpo_mlx_create(hu_allocator_t *alloc,
                               const hu_rl_trainer_config_t *config,
                               hu_rl_trainer_t *out);

/* Phase 4 Task 10: swap the reward source on a GRPO trainer.
 *
 * The HUML backend constructs a synthetic reward source by default at
 * factory time (see hu_grpo_huml_create); this setter lets the CLI
 * replace it with an RM-backed (Phase 3) or judge-backed (Phase 5)
 * source AFTER construction but BEFORE the first step() call. The
 * trainer takes ownership of `source` by value-copy: the previously
 * owned reward source is deinitialized before the swap. The trainer
 * does NOT take ownership of any pointers `source` borrows (e.g. the
 * hu_reward_model_t *rm in an RM-backed source) — those must outlive
 * the trainer.
 *
 * Returns:
 *   HU_OK                — swap completed on a HUML GRPO trainer.
 *   HU_ERR_INVALID_ARGUMENT — NULL trainer / NULL trainer->ctx /
 *                             source.vtable NULL.
 *   HU_ERR_NOT_SUPPORTED — trainer is not the HUML GRPO backend (MLX
 *                          subprocess samples + scores in Python; the
 *                          C side has no reward source to swap). */
hu_error_t hu_grpo_set_reward_source(hu_rl_trainer_t *trainer,
                                      hu_reward_source_t source);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_GRPO_H */
