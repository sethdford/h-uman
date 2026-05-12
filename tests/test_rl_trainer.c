/* tests/test_rl_trainer.c — Phase 2 Task 1
 *
 * Pins the factory dispatch semantics of `hu_rl_trainer_create_dpo`:
 *   1. HUML backend returns a valid vtable. RE-ENABLED in Phase 2 Task 4
 *      now that src/ml/dpo_real_huml.c implements hu_dpo_real_huml_create
 *      for real (commit replacing the stub returning HU_ERR_NOT_SUPPORTED).
 *   2. MLX backend returns HU_ERR_NOT_SUPPORTED when mlx-lm-lora is
 *      unavailable. This exercises the missing-mlx path; it passes by
 *      returning the right error, NOT by being skipped (mlx-lm-lora is
 *      not installed in this environment — explicit per the parent
 *      task brief).
 *   3. AUTO backend falls through to a working backend. STILL DISABLED —
 *      re-enable after Task 6 implements the MLX backend. With Task 4
 *      done the HUML factory works, but on Apple AUTO probes for
 *      mlx-lm-lora first and only falls through to HUML when it's
 *      missing. The probe is environment-dependent (system call to
 *      `python3 -c 'from mlx_lm_lora.trainer.dpo_trainer import train_dpo'`)
 *      and Task 6 will replace it with a deterministic MLX backend. Until
 *      then, leave this test #if 0'd to keep the suite hermetic.
 *
 * Plan deviation note: the canonical plan snippet (lines 240–298)
 * `#include`s `"human/allocator.h"`. That path does not exist in this
 * repo — the real path is `"human/core/allocator.h"`. Using the real
 * path so this test compiles.
 */
#include "test_framework.h"
#include "human/ml/rl_trainer.h"
#include "human/core/allocator.h"

static void test_rl_trainer_factory_huml_returns_valid_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .beta = 0.1,
        .learning_rate = 1e-5,
    };
    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_rl_trainer_create_dpo(&alloc, &cfg, &trainer);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(trainer.vtable);
    HU_ASSERT_NOT_NULL(trainer.vtable->step);
    HU_ASSERT_NOT_NULL(trainer.vtable->save_adapter);
    HU_ASSERT_NOT_NULL(trainer.vtable->name);
    HU_ASSERT_NOT_NULL(trainer.vtable->deinit);
    HU_ASSERT_STR_EQ(trainer.vtable->name(trainer.ctx), "dpo_huml");
    trainer.vtable->deinit(trainer.ctx, &alloc);
}

static void test_rl_trainer_factory_mlx_errors_clearly_when_unavailable(void) {
#if defined(__APPLE__)
    /* On Apple, this is environment-dependent; skip if the third-party
     * mlx-lm-lora package is installed (DPO trainer lives there, NOT in
     * standard mlx-lm — see Task 0 step 2 verification). */
    if (system("python3 -c 'from mlx_lm_lora.trainer.dpo_trainer import train_dpo' 2>/dev/null") == 0) {
        fprintf(stderr, "[skip] mlx-lm-lora present; cannot test unavailability\n");
        return;
    }
#endif
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {.backend = HU_DPO_BACKEND_MLX};
    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_rl_trainer_create_dpo(&alloc, &cfg, &trainer);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(trainer.vtable);
}

#if 0 /* Re-enable after Task 4 OR Task 6 implements at least one real backend */
static void test_rl_trainer_factory_auto_falls_through_when_mlx_unavailable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {.backend = HU_DPO_BACKEND_AUTO, .beta = 0.1};
    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_rl_trainer_create_dpo(&alloc, &cfg, &trainer);
    HU_ASSERT_EQ(err, HU_OK);
    /* Either backend is acceptable; just verify some vtable came back */
    HU_ASSERT_NOT_NULL(trainer.vtable);
    trainer.vtable->deinit(trainer.ctx, &alloc);
}
#endif

void run_rl_trainer_tests(void) {
    HU_RUN_TEST(test_rl_trainer_factory_huml_returns_valid_vtable);
    HU_RUN_TEST(test_rl_trainer_factory_mlx_errors_clearly_when_unavailable);
    /* HU_RUN_TEST(test_rl_trainer_factory_auto_falls_through_when_mlx_unavailable);
     *   Re-enable after Task 6 implements the MLX backend. With Task 4
     *   landed the HUML factory works, but AUTO on Apple probes for
     *   mlx-lm-lora before falling back to HUML — the probe is
     *   environment-dependent and Task 6 will pin it deterministically. */
}
