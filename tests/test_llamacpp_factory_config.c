/*
 * Phase 1 (RL SOTA) — factory entry-point wiring tests.
 *
 * Verifies that hu_provider_create_from_entry forwards every llamacpp
 * tuning field (context_size / threads / use_gpu / n_gpu_layers) plus
 * the model_path (via base_url) into the hu_llamacpp_config_t handed
 * to hu_llamacpp_provider_create.
 *
 * These tests are isolated from libllama: the test hook captures the
 * config struct the factory built and we assert on it directly. No
 * model is loaded, no Metal kernels are dispatched.
 */

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "human/providers/llamacpp.h"
#include "test_framework.h"

#include <string.h>

static hu_allocator_t alloc(void) {
    return hu_system_allocator();
}

static void test_factory_forwards_full_llamacpp_config(void) {
    hu_llamacpp_factory_reset_for_test();

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name         = (char *)"llamacpp",
        .base_url     = (char *)"/tmp/some-model.gguf",
        .context_size = 8192,
        .threads      = 6,
        .use_gpu      = true,
        .n_gpu_layers = 99,
    };
    hu_provider_t prov = {0};

    hu_error_t r = hu_provider_create_from_entry(&a, &entry, &prov);
    HU_ASSERT_EQ(r, HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_NOT_NULL(captured->model_path);
    HU_ASSERT_STR_EQ(captured->model_path, "/tmp/some-model.gguf");
    HU_ASSERT_EQ(captured->context_size, (size_t)8192);
    HU_ASSERT_EQ(captured->threads, 6);
    HU_ASSERT_TRUE(captured->use_gpu);
    HU_ASSERT_EQ(captured->n_gpu_layers, 99);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_llamacpp_dotted_alias(void) {
    /* Both "llamacpp" and "llama.cpp" must dispatch to the same
     * builder — the dotted form is the canonical config-file spelling
     * but our internal name table uses the dotless form. */
    hu_llamacpp_factory_reset_for_test();

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name         = (char *)"llama.cpp",
        .base_url     = (char *)"/tmp/dotted.gguf",
        .context_size = 4096,
    };
    hu_provider_t prov = {0};

    hu_error_t r = hu_provider_create_from_entry(&a, &entry, &prov);
    HU_ASSERT_EQ(r, HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_STR_EQ(captured->model_path, "/tmp/dotted.gguf");
    HU_ASSERT_EQ(captured->context_size, (size_t)4096);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_zero_fields_pass_through(void) {
    /* When the JSON config omits the tuning fields, zero must reach
     * the llamacpp config (which interprets zero as "use llama.cpp's
     * default"). The capture hook lets us prove that. */
    hu_llamacpp_factory_reset_for_test();

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name     = (char *)"llamacpp",
        .base_url = (char *)"/tmp/defaults.gguf",
    };
    hu_provider_t prov = {0};

    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_EQ(captured->context_size, (size_t)0);
    HU_ASSERT_EQ(captured->threads, 0);
    HU_ASSERT_FALSE(captured->use_gpu);
    HU_ASSERT_EQ(captured->n_gpu_layers, 0);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_non_llamacpp_skips_capture(void) {
    /* A non-llamacpp entry must not touch the llamacpp capture state.
     * Use ollama because it doesn't require an api_key to construct. */
    hu_llamacpp_factory_reset_for_test();

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name     = (char *)"ollama",
        .base_url = (char *)"http://localhost:11434",
    };
    hu_provider_t prov = {0};

    hu_error_t r = hu_provider_create_from_entry(&a, &entry, &prov);
    HU_ASSERT_EQ(r, HU_OK);
    HU_ASSERT_NULL(hu_llamacpp_factory_last_config());

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
}

static void test_factory_rejects_null_args(void) {
    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = { .name = (char *)"llamacpp" };
    hu_provider_t prov = {0};

    HU_ASSERT_EQ(hu_provider_create_from_entry(NULL, &entry, &prov), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, NULL, &prov), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, NULL), HU_ERR_INVALID_ARGUMENT);

    hu_provider_entry_t no_name = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &no_name, &prov), HU_ERR_INVALID_ARGUMENT);
}

void run_llamacpp_factory_config_tests(void) {
    HU_RUN_TEST(test_factory_forwards_full_llamacpp_config);
    HU_RUN_TEST(test_factory_llamacpp_dotted_alias);
    HU_RUN_TEST(test_factory_zero_fields_pass_through);
    HU_RUN_TEST(test_factory_non_llamacpp_skips_capture);
    HU_RUN_TEST(test_factory_rejects_null_args);
}
