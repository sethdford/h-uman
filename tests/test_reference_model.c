/* tests/test_reference_model.c — Phase 2 Task 3
 *
 * Verifies hu_reference_model_create_from:
 *   1. At clone time, log π_ref(y|x) == log π_base(y|x) within 1e-5.
 *   2. After perturbing base's first parameter buffer by +0.5, log π_ref
 *      stays bit-identical (the reference's params live in their own
 *      memory and are not mutated when base mutates).
 *
 * Plan deviation notes (same as Tasks 1 + 2):
 *   1. The canonical plan snippet (lines 722-787) `#include`s
 *      `"human/allocator.h"`. That header does not exist in this repo —
 *      the real path is `"human/core/allocator.h"`.
 *   2. The plan's hu_gpt_config_t designated initialiser uses field
 *      names `n_layers`, `n_heads`, `d_model`, `max_seq_len` which do
 *      NOT exist on this repo's struct (see include/human/ml/ml.h:31-44
 *      — the real fields are `n_layer`, `n_head`, `n_kv_head`, `n_embd`,
 *      `head_dim`, `sequence_len`). Mirroring the working initializer
 *      from tests/test_policy_logprobs.c (vocab_size=32, n_layer=1,
 *      n_head=1, n_kv_head=1, n_embd=16, head_dim=16, sequence_len=16),
 *      which compiles + runs at Task 2.
 */
#include "test_framework.h"
#include "human/ml/reference_model.h"
#include "human/ml/policy_logprobs.h"
#include "human/ml/model.h"
#include "human/core/allocator.h"
#include <math.h>

/* π_ref forward at clone time matches base π_θ */
static void test_reference_model_clone_matches_base_at_t0(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.vocab_size = 32;
    cfg.n_layer = 1;
    cfg.n_head = 1;
    cfg.n_kv_head = 1;
    cfg.n_embd = 16;
    cfg.head_dim = 16;
    cfg.sequence_len = 16;
    hu_model_t base = {0}, ref = {0};
    HU_ASSERT_EQ(hu_gpt_create(&alloc, &cfg, &base), HU_OK);
    HU_ASSERT_EQ(hu_reference_model_create_from(&alloc, &base, &cfg, &ref), HU_OK);

    int32_t prompt[] = {1, 2}, response[] = {3, 4};
    double lp_base = 0, lp_ref = 0;
    hu_policy_logprobs(&alloc, &base, prompt, 2, response, 2, &lp_base);
    hu_policy_logprobs(&alloc, &ref,  prompt, 2, response, 2, &lp_ref);

    HU_ASSERT_TRUE(fabs(lp_base - lp_ref) < 1e-5);

    base.vtable->deinit(base.ctx, &alloc);
    ref.vtable->deinit(ref.ctx, &alloc);
}

/* π_ref stays UNCHANGED after π_θ is mutated (perturb base weights manually) */
static void test_reference_model_unchanged_after_base_perturbed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.vocab_size = 32;
    cfg.n_layer = 1;
    cfg.n_head = 1;
    cfg.n_kv_head = 1;
    cfg.n_embd = 16;
    cfg.head_dim = 16;
    cfg.sequence_len = 16;
    hu_model_t base = {0}, ref = {0};
    hu_gpt_create(&alloc, &cfg, &base);
    hu_reference_model_create_from(&alloc, &base, &cfg, &ref);

    int32_t p[] = {1, 2}, r[] = {3, 4};
    double lp_ref_t0 = 0, lp_ref_t1 = 0;
    hu_policy_logprobs(&alloc, &ref, p, 2, r, 2, &lp_ref_t0);

    /* Perturb base via the params buffer.
     * get_params signature per include/human/ml/model.h:32:
     *   hu_error_t (*get_params)(void *ctx, hu_ml_tensor_t **params, size_t *count);
     * Returns a pointer-to-array owned by the model (NOT caller-allocated). */
    hu_ml_tensor_t *params = NULL;
    size_t n_params = 0;
    HU_ASSERT_EQ(base.vtable->get_params(base.ctx, &params, &n_params), HU_OK);
    HU_ASSERT_TRUE(n_params > 0);
    /* Hammer the first float buffer with a known offset.
     * No .n field — element count = size_bytes / sizeof(float) for float tensors. */
    HU_ASSERT_EQ(params[0].dtype, HU_ML_DTYPE_F32);
    float *first = (float *)params[0].data;
    size_t first_count = params[0].size_bytes / sizeof(float);
    for (size_t i = 0; i < first_count; i++) first[i] += 0.5f;

    hu_policy_logprobs(&alloc, &ref, p, 2, r, 2, &lp_ref_t1);
    HU_ASSERT_TRUE(fabs(lp_ref_t0 - lp_ref_t1) < 1e-9);  /* ref UNCHANGED */

    base.vtable->deinit(base.ctx, &alloc);
    ref.vtable->deinit(ref.ctx, &alloc);
}

void run_reference_model_tests(void) {
    HU_RUN_TEST(test_reference_model_clone_matches_base_at_t0);
    HU_RUN_TEST(test_reference_model_unchanged_after_base_perturbed);
}
