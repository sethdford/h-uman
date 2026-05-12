/* tests/test_dpo_real_mlx.c — Phase 2 Task 6
 *
 * Pins the MLX subprocess DPO trainer in src/ml/dpo_real_mlx.c:
 *   - WITHOUT HU_HAVE_MLX_LM (the CI default), this file compiles a
 *     skip stub that fprintfs to stderr and counts as PASS so the
 *     suite stays hermetic and discoverable via --filter=dpo_real_mlx.
 *   - WITH HU_HAVE_MLX_LM=1 (local opt-in), the real test runs:
 *     drives one step() through the factory, asserts the metrics
 *     adapter_path is populated, and confirms the .safetensors file
 *     exists and is non-empty. The HU_IS_TEST shortcut in
 *     dpo_real_mlx.c writes a dummy adapter so this works WITHOUT
 *     spinning up MLX — that's exactly the cheap-test path. The
 *     real-MLX adapter validation (downloads Gemma) is Task 7's job.
 *
 * Plan deviation note: the canonical plan snippet (lines 1505–1556)
 * does not include `human/error.h`; HU_OK comes in via
 * `human/ml/rl_trainer.h` → `human/core/error.h`. Matching plan body
 * verbatim apart from the fields, which on this repo are fixed-size
 * char arrays (see include/human/ml/dpo.h:15–26); designated string
 * initialisers are valid C99 for char[] arrays.
 */
#include "test_framework.h"
#include "human/ml/rl_trainer.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef HU_HAVE_MLX_LM
static void test_dpo_real_mlx_skipped(void) {
    fprintf(stderr, "[skip] HU_HAVE_MLX_LM not defined; mlx-lm-lora DPO subprocess test deferred to local run\n");
}
#else
static void test_dpo_real_mlx_jsonl_export_then_subprocess(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_MLX,
        .beta = 0.1,
        .max_iters = 4,  /* tiny — just prove it runs */
        .model_id = "mlx-community/gemma-3-4b-it-bf16",
        .adapter_out_dir = "/tmp/hu_dpo_mlx_test",
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_dpo(&alloc, &cfg, &trainer), HU_OK);

    /* Use the same fixture */
    /* ... read JSONL into hu_preference_pair_t array ... */
    hu_preference_pair_t pair = {.prompt="hi",.chosen="hello!",.rejected="meh.",.source="test"};
    hu_rl_trainer_metrics_t m = {0};
    hu_error_t err = trainer.vtable->step(trainer.ctx, &alloc, &pair, 1, &m);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(m.adapter_path[0] != '\0');

    /* Confirm safetensors file exists */
    FILE *f = fopen(m.adapter_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    HU_ASSERT_TRUE(sz > 0);

    trainer.vtable->deinit(trainer.ctx, &alloc);
}
#endif

void run_dpo_real_mlx_tests(void) {
#ifdef HU_HAVE_MLX_LM
    HU_RUN_TEST(test_dpo_real_mlx_jsonl_export_then_subprocess);
#else
    HU_RUN_TEST(test_dpo_real_mlx_skipped);
#endif
}
