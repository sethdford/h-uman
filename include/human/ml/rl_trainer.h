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
#ifndef HU_ML_RL_TRAINER_H
#define HU_ML_RL_TRAINER_H

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
    double lambda_d;   /* KTO weight for desirable signal; 0.0 treated as 1.0. DPO+GRPO impls IGNORE. */
    double lambda_u;   /* KTO weight for undesirable signal; 0.0 treated as 1.0. DPO+GRPO impls IGNORE. */
    /* Phase 4 Task 0 (RL SOTA) — GRPO-only fields. DPO+KTO impls IGNORE
     * them (the dispatcher just forwards the whole config struct, and the
     * step functions of those backends never read these fields).  Zero
     * defaults are safe for existing callers. */
    size_t n_rollouts;     /* GRPO rollouts per prompt; 0 treated as default 4
                            * (umbrella §5 ship contract; deviation from trl's
                            * default of 8 — D6 rationale in the Phase 4 plan). */
    double clip_eps;       /* GRPO PPO ratio clip ε; 0.0 treated as default 0.2
                            * (trl/grpo_config.py convention). */
    /* GRPO KL penalty coefficient β (Phase 4 critic R3 MED-1 contract):
     *   kl_beta <  0  → use default 0.04  (DeepSeek R1; umbrella §11 Q10)
     *   kl_beta == 0  → KL penalty DISABLED (R4 escape valve; Task 6
     *                                        finite-diff grad-check uses
     *                                        this to isolate the policy
     *                                        gradient from the KL term)
     *   kl_beta >  0  → use literal value
     * The negative sentinel is required because 0.0 is the canonical
     * "feature off" value used by ablation tests; without the sentinel
     * we'd lose the ability to express "I want the literature default"
     * vs "I want the KL term gone". */
    double kl_beta;
    /* SimPO fields (reference-free DPO). DPO+KTO+GRPO impls IGNORE. */
    double gamma;        /* SimPO reward margin; 0.0 treated as default 0.5 */
    bool length_norm;    /* SimPO length normalization; default false */
    /* ORPO fields (odds-ratio preference optimization). DPO+KTO+GRPO+SimPO impls IGNORE. */
    double lambda_or;    /* ORPO odds-ratio weight; 0.0 treated as default 0.1 */
    double odds_clip;    /* ORPO odds-ratio clip; 0.0 treated as default 10.0 */
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

/* Construct a KTO trainer. Like _create_dpo but consumes one-sided
 * preference pairs (chosen_len > 0 XOR rejected_len > 0). Two-sided
 * pairs are silently skipped at step time. The same backend enum
 * (HUML / MLX / AUTO) gates dispatch. */
hu_error_t hu_rl_trainer_create_kto(hu_allocator_t *alloc,
                                     const hu_rl_trainer_config_t *config,
                                     hu_rl_trainer_t *out);

/* Phase 4 (RL SOTA): Construct a GRPO trainer (Group Relative Policy
 * Optimization, Shao et al. 2024 — DeepSeekMath §4.1.2). Like
 * _create_dpo / _create_kto but uses multi-rollout group-relative
 * baseline + PPO-clipped ratio + optional KL penalty against a frozen
 * reference. Consumes hu_preference_pair_t rows for the PROMPT only —
 * chosen/rejected text is IGNORED because rollouts are sampled from the
 * live policy via hu_rollout_t.
 *
 * Dispatches to hu_grpo_huml_create (HUML in-process toy GPT, Phase 4
 * Task 5) or hu_grpo_mlx_create (MLX subprocess wrapping
 * scripts/grpo_mlx_train.py, Phase 4 Task 8) based on
 * config->backend. AUTO probes mlx-lm-lora's GRPO trainer at runtime
 * and falls back to HUML when unavailable. */
hu_error_t hu_rl_trainer_create_grpo(hu_allocator_t *alloc,
                                      const hu_rl_trainer_config_t *config,
                                      hu_rl_trainer_t *out);

#if HU_IS_TEST
/* Test hooks for inspecting last-resolved backend without spawning a subprocess. */
hu_dpo_backend_t hu_rl_trainer_last_resolved_backend_for_test(void);
void hu_rl_trainer_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_RL_TRAINER_H */
