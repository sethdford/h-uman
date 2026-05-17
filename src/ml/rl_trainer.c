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
#include "human/ml/grpo.h"     /* hu_grpo_huml_create, hu_grpo_mlx_create */
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

/* Probe: KTO trainer importable from mlx-lm-lora. Mirrors the DPO
 * probe above — checks for the specific KTOTrainingArgs symbol path
 * (Phase 3 audit fold-in F2). Generic `import mlx_lm_lora.train`
 * succeeds on partial installs that lack the KTO trainer module,
 * deferring failure to popen() time. */
static int mlx_lm_lora_kto_available(void) {
    return system(
        "python3 -c 'from mlx_lm_lora.trainer.kto_trainer "
        "import train_kto, KTOTrainingArgs' 2>/dev/null") == 0;
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

/* Probe: GRPO trainer importable from mlx-lm-lora. Mirrors the DPO and
 * KTO probes above. The canonical Python symbol is
 *   `mlx_lm_lora.trainer.grpo_trainer.train_grpo`
 * matching scripts/grpo_mlx_train.py (landed by Phase 4 Task 8). If the
 * canonical symbol moves or the package only exposes GRPO via the CLI
 * (`python3 -m mlx_lm_lora.train --train-mode grpo`), the second
 * subprocess catches that — same fallback shape the Phase 2 plan note
 * for scripts/dpo_mlx_train.py describes.
 *
 * fix(plan,grpo,test): under HU_IS_TEST the probe short-circuits to
 * "unavailable" (round-3 critic M7). Unconditionally spawning a probe
 * subprocess from test mode is hostile to the test-deterministic /
 * no-side-effects contract in AGENTS.md §3 + docs/standards/security;
 * tests that need the real probe path opt in via the compile-time
 * HU_HAVE_MLX_LM_GRPO flag and run gated. */
static int mlx_lm_lora_grpo_available(void) {
#if HU_IS_TEST
    return 0;
#else
    if (system("python3 -c 'from mlx_lm_lora.trainer.grpo_trainer "
               "import train_grpo' 2>/dev/null") == 0) {
        return 1;
    }
    return system("python3 -m mlx_lm_lora.train --help 2>/dev/null "
                  "| grep -q 'train-mode\\|grpo'") == 0;
#endif
}

hu_error_t hu_rl_trainer_create_grpo(hu_allocator_t *alloc,
                                      const hu_rl_trainer_config_t *config,
                                      hu_rl_trainer_t *out) {
    if (!alloc || !config || !out) return HU_ERR_INVALID_ARGUMENT;
    hu_dpo_backend_t resolved = config->backend;
    if (resolved == HU_DPO_BACKEND_AUTO) {
#if defined(__APPLE__)
        resolved = mlx_lm_lora_grpo_available() ? HU_DPO_BACKEND_MLX : HU_DPO_BACKEND_HUML;
#else
        resolved = HU_DPO_BACKEND_HUML;
#endif
    }
#if HU_IS_TEST
    s_last_backend = resolved;
#endif
    if (resolved == HU_DPO_BACKEND_HUML) return hu_grpo_huml_create(alloc, config, out);
    if (resolved == HU_DPO_BACKEND_MLX)  return hu_grpo_mlx_create(alloc, config, out);
    return HU_ERR_INVALID_ARGUMENT;
}

/* Phase 4 Task 0 (RL SOTA) — temporary stubs for hu_grpo_huml_create
 * and hu_grpo_mlx_create.  Replaced by:
 *   - Task 5: src/ml/grpo.c     defines HU_GRPO_HAVE_HUML_IMPL at the top
 *   - Task 8: src/ml/grpo_mlx.c defines HU_GRPO_HAVE_MLX_IMPL  at the top
 * When the strong impl TU lands, its `#define` makes the matching stub
 * here fall out via `#ifndef`, leaving exactly one definition per
 * symbol for the linker.
 *
 * fix(plan,grpo,c11): conditional compilation, NOT __attribute__((weak))
 * (round-3 critic L3). The `weak` attribute is a GCC/Clang extension,
 * not standard C11; AGENTS.md §3 mandates strict C11 with
 * -Wall -Wextra -Wpedantic -Werror.  Phase 2 used the same #ifdef
 * pattern for the parallel dpo_real_huml/dpo_real_mlx scaffolding. */
#ifndef HU_GRPO_HAVE_HUML_IMPL
hu_error_t hu_grpo_huml_create(hu_allocator_t *alloc,
                                const hu_rl_trainer_config_t *config,
                                hu_rl_trainer_t *out) {
    (void)alloc; (void)config; (void)out;
    return HU_ERR_NOT_SUPPORTED;
}
#endif

#ifndef HU_GRPO_HAVE_MLX_IMPL
hu_error_t hu_grpo_mlx_create(hu_allocator_t *alloc,
                               const hu_rl_trainer_config_t *config,
                               hu_rl_trainer_t *out) {
    (void)alloc; (void)config; (void)out;
    return HU_ERR_NOT_SUPPORTED;
}
#endif
