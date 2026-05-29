/* tests/test_rl_trainer.c — Phase 2 Task 1
 *
 * Pins the factory dispatch semantics of `hu_rl_trainer_create_dpo`:
 *   1. HUML backend returns a valid vtable. RE-ENABLED in Phase 2 Task 4
 *      now that src/ml/dpo_real_huml.c implements hu_dpo_real_huml_create
 *      for real (commit replacing the stub returning HU_ERR_NOT_SUPPORTED).
 *   2. MLX backend returns HU_ERR_NOT_SUPPORTED when mlx-lm-lora is
 *      unavailable. Phase 3 Task 0 (D6) tightened this from the
 *      original Phase 2 form: previously the test skipped when the
 *      third-party mlx-lm-lora package was installed in the ambient
 *      environment (PATH-dependent skip), leaving the unavailability
 *      path uncovered. Now hu_dpo_real_mlx_create probes at CREATE
 *      time (not step time), and the test drives a deterministic
 *      miss by overriding PATH to /var/empty regardless of whether
 *      mlx-lm-lora is installed.
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
#include "human/core/allocator.h"
#include "human/ml/rl_trainer.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>

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
    /* Phase 3 Task 0 (D6): deterministic unavailability test.
     *
     * Previously this case skipped whenever the third-party mlx-lm-lora
     * package was installed in the ambient environment, leaving the
     * "no mlx-lm-lora" code path uncovered. After Task 0's create-time
     * probe fold-in, hu_dpo_real_mlx_create calls
     * `system("python3 -c '...' 2>/dev/null")` BEFORE allocating, so we
     * can drive it to a deterministic miss by overriding PATH to a
     * directory that contains no python3. Both /var/empty (macOS) and
     * /var/empty (Linux) exist and are guaranteed empty of executables.
     *
     * Pin both probe sites simultaneously:
     *   1. src/ml/rl_trainer.c::mlx_dpo_available() — used by AUTO
     *      backend dispatch; with PATH=/var/empty it returns 0 and
     *      AUTO would have routed to HUML. We bypass that here by
     *      requesting HU_DPO_BACKEND_MLX explicitly so the create
     *      function's own probe is exercised.
     *   2. src/ml/dpo_real_mlx.c::mlx_lm_lora_available() — the new
     *      create-time probe; with PATH=/var/empty it returns 0 and
     *      hu_dpo_real_mlx_create returns HU_ERR_NOT_SUPPORTED cleanly.
     *
     * On non-Apple builds the create function short-circuits to
     * HU_ERR_NOT_SUPPORTED via `#if !defined(__APPLE__)` before the
     * probe runs, so the assertion still holds regardless of PATH. */
    const char *saved_path = getenv("PATH");
    char saved_copy[4096] = {0};
    if (saved_path)
        snprintf(saved_copy, sizeof(saved_copy), "%s", saved_path);
    setenv("PATH", "/var/empty", 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {.backend = HU_DPO_BACKEND_MLX};
    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_rl_trainer_create_dpo(&alloc, &cfg, &trainer);

    if (saved_copy[0])
        setenv("PATH", saved_copy, 1);
    else
        unsetenv("PATH");

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
    /* The AUTO-fallback test (test_rl_trainer_factory_auto_falls_through_when_
     * mlx_unavailable) is intentionally NOT registered yet — deferred to RL
     * Task 6 (MLX backend). With Task 4 landed the HUML factory works, but AUTO
     * on Apple probes for mlx-lm-lora before falling back to HUML, and that
     * probe is environment-dependent; Task 6 will pin it deterministically.
     * Expressed in prose (not a commented-out HU_RUN_TEST line) so it isn't a
     * silent coverage hole — see scripts/check-disabled-test-registration.sh. */
}
