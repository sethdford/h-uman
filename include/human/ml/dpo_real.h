/* include/human/ml/dpo_real.h
 *
 * Shared public header for the two-track DPO real-training backends
 * dispatched by `hu_rl_trainer_create_dpo` (see human/ml/rl_trainer.h):
 *
 *   - HUML (in-process, toy GPT, gradient-checked, cross-platform)
 *   - MLX  (Apple-only subprocess wrapping the third-party
 *           `mlx-lm-lora` package — DPO is NOT in standard mlx-lm)
 *
 * Phase 2 Task 1 stubs both factories at HU_ERR_NOT_SUPPORTED; Tasks 4
 * and 6 fill them in.
 *
 * Plan deviation note: the canonical plan snippet (lines 902–935) shows
 * `#include "human/allocator.h"` / `"human/error.h"`. Those headers do
 * not exist in this repo — the real paths are `human/core/allocator.h`
 * and `human/core/error.h` (confirmed against every existing header in
 * `include/human/ml/` and every test under `tests/`). Using the real
 * paths so this file compiles.
 */
#ifndef HU_ML_DPO_REAL_H
#define HU_ML_DPO_REAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/model.h"
#include "human/ml/rl_trainer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Construct an in-process HUML DPO trainer (toy GPT, cross-platform,
 * gradient-checked, NOT for improving real Gemma chat). Implements the
 * hu_rl_trainer_vtable_t and is dispatched by hu_rl_trainer_create_dpo
 * when backend == HUML or AUTO falls back to it. */
hu_error_t hu_dpo_real_huml_create(hu_allocator_t *alloc,
                                    const hu_rl_trainer_config_t *config,
                                    hu_rl_trainer_t *out);

/* Construct an MLX subprocess DPO trainer (Apple-only, requires
 * mlx-lm-lora package — pip install mlx-lm-lora). Outputs a real
 * .safetensors LoRA adapter that llama.cpp hot-loads. Returns
 * HU_ERR_NOT_SUPPORTED on non-Apple platforms or when the
 * mlx-lm-lora package is unavailable. */
hu_error_t hu_dpo_real_mlx_create(hu_allocator_t *alloc,
                                   const hu_rl_trainer_config_t *config,
                                   hu_rl_trainer_t *out);

#if HU_IS_TEST
/* Shared policy model for E2E mock provider wiring. */
const hu_model_t *hu_dpo_real_huml_policy_for_test(const hu_rl_trainer_t *trainer);
#endif

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_DPO_REAL_H */
