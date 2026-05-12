/* include/human/ml/rl_trainer.h
 *
 * Phase 2 Task 1: new abstraction for RL training (DPO now, KTO in
 * Phase 3, GRPO in Phase 4). Factory dispatches to either an in-process
 * HUML backend (cross-platform, toy GPT, gradient-checked) or an MLX
 * subprocess backend (Apple-only, real Gemma adapters via the
 * third-party `mlx-lm-lora` PyPI package, .safetensors output).
 *
 * Both backends are stubbed at HU_ERR_NOT_SUPPORTED in this commit;
 * Tasks 4 and 6 fill them in. The factory's auto/huml/mlx selection
 * logic is real and tested here.
 *
 * Plan deviation note: the canonical plan snippet (lines 308–372) shows
 * `#include "human/allocator.h"` / `"human/error.h"`. Those headers do
 * not exist in this repo — the real paths are `human/core/allocator.h`
 * and `human/core/error.h`. Using the real paths so this file compiles.
 */
#ifndef HUMAN_ML_RL_TRAINER_H
#define HUMAN_ML_RL_TRAINER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"  /* hu_preference_pair_t */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HU_DPO_BACKEND_AUTO = 0,
    HU_DPO_BACKEND_HUML = 1,
    HU_DPO_BACKEND_MLX  = 2,
} hu_dpo_backend_t;

typedef struct {
    hu_dpo_backend_t backend;
    double beta;            /* DPO temperature; default 0.1 */
    double learning_rate;   /* default 1e-5 (HUML) or ignored (MLX) */
    size_t max_iters;       /* default 100 */
    const char *model_id;   /* MLX: HF id like "mlx-community/gemma-3-4b-it-bf16"; HUML: ignored */
    const char *adapter_out_dir; /* MLX: writes adapters.safetensors here; HUML: ignored */
} hu_rl_trainer_config_t;

typedef struct {
    double final_loss;
    size_t iters_completed;
    double chosen_logprob_delta;  /* HUML only; MLX leaves 0 */
    double rejected_logprob_delta;
    char adapter_path[512];       /* MLX writes here; HUML leaves empty */
} hu_rl_trainer_metrics_t;

typedef struct hu_rl_trainer_vtable {
    hu_error_t (*step)(void *ctx, hu_allocator_t *alloc,
                       const hu_preference_pair_t *pairs, size_t n_pairs,
                       hu_rl_trainer_metrics_t *out);
    hu_error_t (*save_adapter)(void *ctx, hu_allocator_t *alloc, const char *path);
    const char *(*name)(void *ctx);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_rl_trainer_vtable_t;

typedef struct {
    void *ctx;
    const hu_rl_trainer_vtable_t *vtable;
} hu_rl_trainer_t;

hu_error_t hu_rl_trainer_create_dpo(hu_allocator_t *alloc,
                                     const hu_rl_trainer_config_t *config,
                                     hu_rl_trainer_t *out);

#ifdef HU_IS_TEST
/* Test hooks for inspecting last-resolved backend without spawning a subprocess. */
hu_dpo_backend_t hu_rl_trainer_last_resolved_backend_for_test(void);
void hu_rl_trainer_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
#endif /* HUMAN_ML_RL_TRAINER_H */
