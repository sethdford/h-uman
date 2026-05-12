/* src/ml/dpo_real_mlx.c — STUB until Phase 2 Task 6
 *
 * Task 6 replaces this with the MLX subprocess DPO trainer (popen of
 * scripts/dpo_mlx_train.py, which imports `train_dpo` from the
 * third-party `mlx-lm-lora` package and writes adapters.safetensors).
 * Until then, the factory returns HU_ERR_NOT_SUPPORTED on every
 * platform — the unavailable-environment branch of rl_trainer's
 * `auto` resolver still works because that branch only consults
 * `mlx_dpo_available()` before getting here.
 */
#include "human/ml/dpo_real.h"

hu_error_t hu_dpo_real_mlx_create(hu_allocator_t *a, const hu_rl_trainer_config_t *c, hu_rl_trainer_t *o) {
    (void)a; (void)c; (void)o; return HU_ERR_NOT_SUPPORTED;
}
