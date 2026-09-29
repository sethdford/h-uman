/*
 * LoRA adapter tests.
 */
#include "human/core/allocator.h"
#include "human/ml/lora.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_ML

static void lora_create_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t cfg = {
        .rank = 4, .alpha = 8.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_ALL};
    hu_lora_adapter_t *adapter = NULL;

    hu_error_t err = hu_lora_create(&alloc, &cfg, 8, 8, 2, &adapter);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(adapter);

    hu_lora_destroy(&alloc, adapter);
}

static void lora_param_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t cfg = {
        .rank = 4, .alpha = 8.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_ALL};
    hu_lora_adapter_t *adapter = NULL;

    HU_ASSERT_EQ(hu_lora_create(&alloc, &cfg, 8, 8, 2, &adapter), HU_OK);
    size_t n = hu_lora_num_params(adapter);
    /* Per layer: rank*in_dim + out_dim*rank = 4*8 + 8*4 = 64. Two layers = 128. */
    HU_ASSERT_EQ(n, 128);

    hu_lora_destroy(&alloc, adapter);
}

static void lora_invalid_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t cfg = {
        .rank = 0, .alpha = 8.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_ALL};
    hu_lora_adapter_t *adapter = NULL;

    hu_error_t err = hu_lora_create(&alloc, &cfg, 8, 8, 2, &adapter);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(adapter);
}

static void lora_destroy_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_destroy(&alloc, NULL);
}

static void lora_apply_adds_delta(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t cfg = {
        .rank = 2, .alpha = 2.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_ALL};
    hu_lora_adapter_t *adapter = NULL;

    HU_ASSERT_EQ(hu_lora_create(&alloc, &cfg, 4, 4, 1, &adapter), HU_OK);

    /* Set A and B to known values. A[2x4], B[4x2]. scale = 2/2 = 1.0 */
    float A[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    float B[8] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    HU_ASSERT_EQ(hu_lora_set_layer_weights(adapter, 0, A, B), HU_OK);

    float input[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float output[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    HU_ASSERT_EQ(hu_lora_apply(adapter, 0, input, 1, output), HU_OK);

    /* temp = input * A^T: [1,2,3,4] * A^T. Row 0 of A^T = [1,0,0,0], row 1 = [0,1,0,0].
     * temp[0] = 1*1+2*0+3*0+4*0 = 1, temp[1] = 1*0+2*1+3*0+4*0 = 2.
     * delta = temp * B^T: [1,2] * B^T. B^T = [[1,0],[0,1],[0,0],[0,0]].
     * delta = [1*1+2*0, 1*0+2*1, 0, 0] = [1, 2, 0, 0]. scale=1.
     * output += [1,2,0,0] = [1,2,0,0] */
    HU_ASSERT_FLOAT_EQ(output[0], 1.0f, 0.001f);
    HU_ASSERT_FLOAT_EQ(output[1], 2.0f, 0.001f);
    HU_ASSERT_FLOAT_EQ(output[2], 0.0f, 0.001f);
    HU_ASSERT_FLOAT_EQ(output[3], 0.0f, 0.001f);

    hu_lora_destroy(&alloc, adapter);
}

static void lora_backward_accumulates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t cfg = {
        .rank = 2, .alpha = 1.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_ALL};
    hu_lora_adapter_t *adapter = NULL;

    HU_ASSERT_EQ(hu_lora_create(&alloc, &cfg, 4, 4, 1, &adapter), HU_OK);

    float input[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float grad_output[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    HU_ASSERT_EQ(hu_lora_backward(adapter, 0, input, grad_output, 1, NULL), HU_OK);

    /* Gradients should be non-zero (we can't inspect them directly, but backward succeeded) */
    hu_lora_destroy(&alloc, adapter);
}

static void lora_target_flags(void) {
    HU_ASSERT_EQ((int)(HU_LORA_TARGET_Q | HU_LORA_TARGET_V), (int)HU_LORA_TARGET_QV);
    HU_ASSERT_EQ((int)HU_LORA_TARGET_ALL, 0x3F);
}

static void lora_backward_grad_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t cfg = {
        .rank = 2, .alpha = 2.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_ALL};
    hu_lora_adapter_t *adapter = NULL;
    HU_ASSERT_EQ(hu_lora_create(&alloc, &cfg, 4, 4, 1, &adapter), HU_OK);

    /* A = [[1,0,0,0],[0,1,0,0]], B = [[1,0],[0,1],[0,0],[0,0]], scale = 2/2 = 1.0
     * LoRA forward: output += (input @ A^T @ B^T) * scale
     * d_input = scale * grad_output @ B @ A */
    float A[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    float B[8] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    HU_ASSERT_EQ(hu_lora_set_layer_weights(adapter, 0, A, B), HU_OK);

    float input[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float grad_output[4] = {1.0f, 0.5f, 0.0f, 0.0f};
    float grad_input[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    HU_ASSERT_EQ(hu_lora_backward(adapter, 0, input, grad_output, 1, grad_input), HU_OK);

    /* grad_output @ B = [1,0.5,0,0] @ [[1,0],[0,1],[0,0],[0,0]] = [1, 0.5]
     * gob @ A = [1, 0.5] @ [[1,0,0,0],[0,1,0,0]] = [1, 0.5, 0, 0] * scale=1 */
    HU_ASSERT_FLOAT_EQ(grad_input[0], 1.0f, 1e-5f);
    HU_ASSERT_FLOAT_EQ(grad_input[1], 0.5f, 1e-5f);
    HU_ASSERT_FLOAT_EQ(grad_input[2], 0.0f, 1e-5f);
    HU_ASSERT_FLOAT_EQ(grad_input[3], 0.0f, 1e-5f);

    hu_lora_destroy(&alloc, adapter);
}

static void lora_backward_grad_input_finite_diff(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t cfg = {
        .rank = 2, .alpha = 4.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_ALL};
    hu_lora_adapter_t *adapter = NULL;
    HU_ASSERT_EQ(hu_lora_create(&alloc, &cfg, 4, 3, 1, &adapter), HU_OK);

    float A[8] = {0.5f, -0.3f, 0.1f, 0.4f, -0.2f, 0.6f, -0.1f, 0.3f};
    float B[6] = {0.3f, -0.5f, 0.2f, 0.4f, -0.1f, 0.7f};
    HU_ASSERT_EQ(hu_lora_set_layer_weights(adapter, 0, A, B), HU_OK);

    float input[4] = {1.0f, -0.5f, 0.3f, 0.8f};
    const size_t in_dim = 4, out_dim = 3;

    /* Forward to get base output */
    float output_base[3] = {0.0f, 0.0f, 0.0f};
    HU_ASSERT_EQ(hu_lora_apply(adapter, 0, input, 1, output_base), HU_OK);

    /* Use unit grad_output for each output dimension to build full Jacobian,
     * then compute grad_input = sum_j grad_output[j] * d(output[j])/d(input[k]) */
    float grad_input_analytical[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float grad_output_full[3] = {1.0f, 1.0f, 1.0f};
    HU_ASSERT_EQ(hu_lora_backward(adapter, 0, input, grad_output_full, 1, grad_input_analytical),
                 HU_OK);

    /* Numerical gradient via finite differences */
    const float eps = 1e-3f;
    for (size_t k = 0; k < in_dim; k++) {
        float saved = input[k];

        input[k] = saved + eps;
        float out_plus[3] = {0.0f, 0.0f, 0.0f};
        HU_ASSERT_EQ(hu_lora_apply(adapter, 0, input, 1, out_plus), HU_OK);

        input[k] = saved - eps;
        float out_minus[3] = {0.0f, 0.0f, 0.0f};
        HU_ASSERT_EQ(hu_lora_apply(adapter, 0, input, 1, out_minus), HU_OK);

        input[k] = saved;

        float numerical = 0.0f;
        for (size_t j = 0; j < out_dim; j++)
            numerical += (out_plus[j] - out_minus[j]) / (2.0f * eps);

        HU_ASSERT_FLOAT_EQ(grad_input_analytical[k], numerical, 1e-2f);
    }

    hu_lora_destroy(&alloc, adapter);
}

static void lora_partial_set_weights(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t cfg = {
        .rank = 2, .alpha = 2.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_ALL};
    hu_lora_adapter_t *adapter = NULL;
    HU_ASSERT_EQ(hu_lora_create(&alloc, &cfg, 4, 4, 1, &adapter), HU_OK);

    float A[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    HU_ASSERT_EQ(hu_lora_set_layer_weights(adapter, 0, A, NULL), HU_OK);

    float B[8] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    HU_ASSERT_EQ(hu_lora_set_layer_weights(adapter, 0, NULL, B), HU_OK);

    float input[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float output[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    HU_ASSERT_EQ(hu_lora_apply(adapter, 0, input, 1, output), HU_OK);
    HU_ASSERT_FLOAT_EQ(output[0], 1.0f, 0.001f);
    HU_ASSERT_FLOAT_EQ(output[1], 2.0f, 0.001f);

    hu_lora_destroy(&alloc, adapter);
}

static void lora_save_load_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t cfg = {
        .rank = 2, .alpha = 4.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_QV};
    hu_lora_adapter_t *adapter = NULL;
    HU_ASSERT_EQ(hu_lora_create(&alloc, &cfg, 4, 4, 2, &adapter), HU_OK);

    float A0[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    float B0[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    float A1[8] = {-1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f, -7.0f, -8.0f};
    float B1[8] = {-0.1f, -0.2f, -0.3f, -0.4f, -0.5f, -0.6f, -0.7f, -0.8f};
    HU_ASSERT_EQ(hu_lora_set_layer_weights(adapter, 0, A0, B0), HU_OK);
    HU_ASSERT_EQ(hu_lora_set_layer_weights(adapter, 1, A1, B1), HU_OK);

    char path[512];
    HU_ASSERT_TRUE(hu_test_tmppath(path, sizeof(path), "hu_lora_roundtrip.bin"));
    HU_ASSERT_EQ(hu_lora_save(adapter, path), HU_OK);

    /* Verify file was actually written */
    FILE *f = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(f);
    char magic[4];
    HU_ASSERT_EQ(fread(magic, 1, 4, f), 4u);
    HU_ASSERT_EQ(memcmp(magic, "LORA", 4), 0);
    fclose(f);

    /* Load and verify weights match */
    hu_lora_adapter_t *loaded = NULL;
    HU_ASSERT_EQ(hu_lora_load(&alloc, path, &loaded), HU_OK);
    HU_ASSERT_NOT_NULL(loaded);

    /* Apply both adapters to same input — outputs must match */
    float input[4] = {1.0f, -0.5f, 0.3f, 0.8f};
    float out_orig[4] = {0}, out_loaded[4] = {0};
    HU_ASSERT_EQ(hu_lora_apply(adapter, 0, input, 1, out_orig), HU_OK);
    HU_ASSERT_EQ(hu_lora_apply(loaded, 0, input, 1, out_loaded), HU_OK);
    for (int i = 0; i < 4; i++)
        HU_ASSERT_FLOAT_EQ(out_orig[i], out_loaded[i], 1e-6f);

    memset(out_orig, 0, sizeof(out_orig));
    memset(out_loaded, 0, sizeof(out_loaded));
    HU_ASSERT_EQ(hu_lora_apply(adapter, 1, input, 1, out_orig), HU_OK);
    HU_ASSERT_EQ(hu_lora_apply(loaded, 1, input, 1, out_loaded), HU_OK);
    for (int i = 0; i < 4; i++)
        HU_ASSERT_FLOAT_EQ(out_orig[i], out_loaded[i], 1e-6f);

    hu_lora_destroy(&alloc, loaded);
    hu_lora_destroy(&alloc, adapter);
    remove(path);
}

static void lora_load_bad_file(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    HU_ASSERT_TRUE(hu_test_tmppath(path, sizeof(path), "hu_lora_bad.bin"));

    /* Write garbage */
    FILE *f = fopen(path, "wb");
    const char bad[] = "NOT_LORA_FILE";
    fwrite(bad, 1, sizeof(bad), f);
    fclose(f);

    hu_lora_adapter_t *loaded = NULL;
    HU_ASSERT_EQ(hu_lora_load(&alloc, path, &loaded), HU_ERR_IO);
    HU_ASSERT_NULL(loaded);

    /* Non-existent file */
    HU_ASSERT_EQ(hu_lora_load(&alloc, "/tmp/nonexistent_lora_xyz.bin", &loaded), HU_ERR_IO);

    remove(path);
}

static void lora_invalid_layer_idx(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t cfg = {
        .rank = 2, .alpha = 2.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_ALL};
    hu_lora_adapter_t *adapter = NULL;
    HU_ASSERT_EQ(hu_lora_create(&alloc, &cfg, 4, 4, 2, &adapter), HU_OK);

    float input[4] = {1, 2, 3, 4};
    float output[4] = {0};
    float grad_out[4] = {1, 1, 1, 1};

    /* Valid indices: 0 and 1 */
    HU_ASSERT_EQ(hu_lora_apply(adapter, 0, input, 1, output), HU_OK);
    HU_ASSERT_EQ(hu_lora_apply(adapter, 1, input, 1, output), HU_OK);

    /* Out of bounds */
    HU_ASSERT_EQ(hu_lora_apply(adapter, 2, input, 1, output), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_lora_apply(adapter, 999, input, 1, output), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_lora_backward(adapter, 2, input, grad_out, 1, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_lora_set_layer_weights(adapter, 2, NULL, NULL), HU_ERR_INVALID_ARGUMENT);

    hu_lora_destroy(&alloc, adapter);
}

/* Failing allocator for OOM testing */
static int oom_alloc_count;
static int oom_fail_at;

static void *oom_alloc(void *ctx, size_t size) {
    (void)ctx;
    if (oom_fail_at >= 0 && oom_alloc_count >= oom_fail_at)
        return NULL;
    oom_alloc_count++;
    return malloc(size);
}

static void oom_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

static void lora_oom_create(void) {
    hu_lora_config_t cfg = {
        .rank = 2, .alpha = 2.0f, .dropout = 0.0f, .targets = HU_LORA_TARGET_ALL};
    hu_lora_adapter_t *adapter = NULL;

    /* Fail on first alloc (adapter struct) */
    hu_allocator_t bad = {.alloc = oom_alloc, .free = oom_free, .ctx = NULL};
    oom_alloc_count = 0;
    oom_fail_at = 0;
    HU_ASSERT_EQ(hu_lora_create(&bad, &cfg, 4, 4, 1, &adapter), HU_ERR_OUT_OF_MEMORY);
    HU_ASSERT_NULL(adapter);

    /* Fail on second alloc (layers array) */
    oom_alloc_count = 0;
    oom_fail_at = 1;
    HU_ASSERT_EQ(hu_lora_create(&bad, &cfg, 4, 4, 1, &adapter), HU_ERR_OUT_OF_MEMORY);
    HU_ASSERT_NULL(adapter);

    /* Fail partway through layer allocation */
    oom_alloc_count = 0;
    oom_fail_at = 4;
    HU_ASSERT_EQ(hu_lora_create(&bad, &cfg, 4, 4, 1, &adapter), HU_ERR_OUT_OF_MEMORY);
    HU_ASSERT_NULL(adapter);
}

void run_lora_tests(void) {
    HU_TEST_SUITE("LoRA Adapter");
    HU_RUN_TEST(lora_create_basic);
    HU_RUN_TEST(lora_param_count);
    HU_RUN_TEST(lora_invalid_config);
    HU_RUN_TEST(lora_destroy_null);
    HU_RUN_TEST(lora_apply_adds_delta);
    HU_RUN_TEST(lora_backward_accumulates);
    HU_RUN_TEST(lora_backward_grad_input);
    HU_RUN_TEST(lora_backward_grad_input_finite_diff);
    HU_RUN_TEST(lora_partial_set_weights);
    HU_RUN_TEST(lora_target_flags);
    HU_RUN_TEST(lora_invalid_layer_idx);
    HU_RUN_TEST(lora_save_load_roundtrip);
    HU_RUN_TEST(lora_load_bad_file);
    HU_RUN_TEST(lora_oom_create);
}

#else

void run_lora_tests(void) {
    (void)0;
}

#endif
