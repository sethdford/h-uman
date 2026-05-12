/* src/ml/rl_trainer.c
 *
 * Phase 2 Task 1: factory dispatch for hu_rl_trainer_t (see
 * include/human/ml/rl_trainer.h). The HUML and MLX backends themselves
 * are stubbed in src/ml/dpo_real_huml.c and src/ml/dpo_real_mlx.c until
 * Tasks 4 and 6 fill them in.
 *
 * AUTO backend probes for the third-party `mlx-lm-lora` package at
 * runtime (the DPO trainer lives at `mlx_lm_lora.trainer.dpo_trainer`
 * — it is NOT in standard `mlx-lm`). When unavailable, falls through
 * to HUML. The probe matches the symbol that scripts/dpo_mlx_train.py
 * imports in Task 6, so they cannot drift.
 */
#include "human/ml/rl_trainer.h"
#include "human/ml/dpo_real.h"  /* hu_dpo_real_huml_create, hu_dpo_real_mlx_create */
#include "human/ml/kto.h"      /* hu_kto_huml_create, hu_kto_mlx_create */
#include "human/core/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HU_IS_TEST
static hu_dpo_backend_t s_last_backend = HU_DPO_BACKEND_AUTO;
hu_dpo_backend_t hu_rl_trainer_last_resolved_backend_for_test(void) { return s_last_backend; }
void hu_rl_trainer_reset_for_test(void) { s_last_backend = HU_DPO_BACKEND_AUTO; }
#endif

static int mlx_dpo_available(void) {
    /* Probe the THIRD-PARTY mlx-lm-lora package (NOT standard mlx-lm).
     * The DPO trainer is `mlx_lm_lora.trainer.dpo_trainer.train_dpo`,
     * matching the symbol our scripts/dpo_mlx_train.py wrapper imports
     * and Task 0 step 2's verification check. Probing the wrong module
     * (`mlx_lm.dpo`) here would make AUTO never resolve to MLX. */
    return system("python3 -c 'from mlx_lm_lora.trainer.dpo_trainer import train_dpo' 2>/dev/null") == 0;
}

hu_error_t hu_rl_trainer_create_dpo(hu_allocator_t *alloc,
                                     const hu_rl_trainer_config_t *config,
                                     hu_rl_trainer_t *out) {
    if (!alloc || !config || !out) return HU_ERR_INVALID_ARGUMENT;
    hu_dpo_backend_t resolved = config->backend;
    if (resolved == HU_DPO_BACKEND_AUTO) {
#if defined(__APPLE__)
        resolved = mlx_dpo_available() ? HU_DPO_BACKEND_MLX : HU_DPO_BACKEND_HUML;
#else
        resolved = HU_DPO_BACKEND_HUML;
#endif
    }
#if HU_IS_TEST
    s_last_backend = resolved;
#endif
    if (resolved == HU_DPO_BACKEND_HUML) return hu_dpo_real_huml_create(alloc, config, out);
    if (resolved == HU_DPO_BACKEND_MLX)  return hu_dpo_real_mlx_create(alloc, config, out);
    return HU_ERR_INVALID_ARGUMENT;
}

/* Probe: KTO trainer importable from mlx-lm-lora. Returns 0 for now —
 * Task 7 fills in the real symbol path. */
static int mlx_lm_lora_kto_available(void) {
    return 0;
}

hu_error_t hu_rl_trainer_create_kto(hu_allocator_t *alloc,
                                     const hu_rl_trainer_config_t *config,
                                     hu_rl_trainer_t *out) {
    if (!alloc || !config || !out) return HU_ERR_INVALID_ARGUMENT;
    hu_dpo_backend_t resolved = config->backend;
    if (resolved == HU_DPO_BACKEND_AUTO) {
#if defined(__APPLE__)
        resolved = mlx_lm_lora_kto_available() ? HU_DPO_BACKEND_MLX : HU_DPO_BACKEND_HUML;
#else
        resolved = HU_DPO_BACKEND_HUML;
#endif
    }
#if HU_IS_TEST
    s_last_backend = resolved;
#endif
    if (resolved == HU_DPO_BACKEND_HUML) return hu_kto_huml_create(alloc, config, out);
    if (resolved == HU_DPO_BACKEND_MLX)  return hu_kto_mlx_create(alloc, config, out);
    return HU_ERR_INVALID_ARGUMENT;
}
