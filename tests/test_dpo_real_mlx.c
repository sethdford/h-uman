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

#ifdef HU_HAVE_MLX_LM
#include "human/providers/llamacpp.h"
#include <string.h>
#endif

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

    /* hu_preference_pair_t carries fixed-size char arrays AND parallel
     * `_len` fields (see include/human/ml/dpo.h:15–26). The C-side
     * write_jsonl in src/ml/dpo_real_mlx.c skips any pair where
     * prompt_len==0 OR both chosen_len and rejected_len are 0 — that's
     * how it filters partial rows pulled from the dpo_pairs sqlite
     * table. Designated initialisers zero-fill the unset members, so
     * we MUST set the _len fields explicitly here; otherwise the JSONL
     * fed to mlx-lm-lora is empty and the CLI bails out with
     * "Training set not found or empty". (The Task 6 plan snippet at
     * lines 1505–1556 omitted these — pinning them here is part of
     * Task 7's e2e gate.) */
    hu_preference_pair_t pair = {
        .prompt = "hi",      .prompt_len = 2,
        .chosen = "hello!",  .chosen_len = 6,
        .rejected = "meh.",  .rejected_len = 4,
        .source = "test",    .source_len = 4,
    };
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

/* Phase 2 Task 7: round-trip the MLX-produced .safetensors through the
 * llama.cpp provider's adapter loader. Gated by HU_HAVE_MLX_LM (compile
 * flag) AND `HU_GEMMA_GGUF` (env var) so the heavy Gemma download
 * isn't forced on environments lacking either. Tests:
 *   1. dpo_mlx_step writes a real safetensors file (via mlx-lm-lora CLI).
 *   2. hu_llamacpp_provider_create binds the GGUF.
 *   3. The llama.cpp vtable's load_adapter/unload_adapter accept the
 *      MLX-trained safetensors without crashing.
 *
 * Plan deviation notes (snippet lives at docs/plans/...lines 1865–1886):
 *   - `load_adapter` in this repo carries (adapter_path, path_len,
 *     adapter_id, id_len) — six args total — not the snippet's
 *     (adapter_path, strength) 4-arg form. We translate to the real
 *     signature in `include/human/provider.h`.
 *   - `unload_adapter` similarly takes (adapter_id, id_len), not
 *     (alloc). We pass the same id we used for load_adapter.
 *   - We gate on HU_GEMMA_GGUF at runtime (skip without failing) so the
 *     test is locally-driven only — matches the Task 7 expected-skip
 *     behaviour at the bottom of the plan snippet.
 */
static void test_dpo_real_mlx_safetensors_loads_in_llamacpp(void) {
    const char *gguf = getenv("HU_GEMMA_GGUF");
    if (!gguf || !*gguf) {
        fprintf(stderr, "[skip] HU_GEMMA_GGUF unset; llama.cpp adapter round-trip "
                        "deferred to local run with the Gemma GGUF on disk\n");
        return;
    }

    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_MLX,
        .beta = 0.1,
        .max_iters = 4,
        .model_id = "mlx-community/gemma-3-4b-it-bf16",
        .adapter_out_dir = "/tmp/hu_dpo_mlx_validation",
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_dpo(&alloc, &cfg, &t), HU_OK);

    /* See the Task 6 pair note above: _len fields must be set or the
     * C-side JSONL exporter skips the row. */
    hu_preference_pair_t p = {
        .prompt = "hi",       .prompt_len = 2,
        .chosen = "hello",    .chosen_len = 5,
        .rejected = "hmph",   .rejected_len = 4,
        .source = "test",     .source_len = 4,
    };
    hu_rl_trainer_metrics_t m = {0};
    HU_ASSERT_EQ(t.vtable->step(t.ctx, &alloc, &p, 1, &m), HU_OK);
    HU_ASSERT_TRUE(m.adapter_path[0] != '\0');

    /* Load adapter into llama.cpp and verify it doesn't crash. */
    hu_llamacpp_config_t lcfg = {
        .model_path = (char *)gguf,
        .context_size = 512,
        .threads = 4,
        .use_gpu = true,
        .n_gpu_layers = -1,
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&alloc, &lcfg, &prov), HU_OK);
    const char *adapter_id = "test-mlx";
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &alloc,
                                           m.adapter_path, strlen(m.adapter_path),
                                           adapter_id, strlen(adapter_id)),
                 HU_OK);
    if (prov.vtable->unload_adapter)
        prov.vtable->unload_adapter(prov.ctx, adapter_id, strlen(adapter_id));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
    t.vtable->deinit(t.ctx, &alloc);
}
#endif

void run_dpo_real_mlx_tests(void) {
#ifdef HU_HAVE_MLX_LM
    HU_RUN_TEST(test_dpo_real_mlx_jsonl_export_then_subprocess);
    HU_RUN_TEST(test_dpo_real_mlx_safetensors_loads_in_llamacpp);
#else
    HU_RUN_TEST(test_dpo_real_mlx_skipped);
#endif
}
