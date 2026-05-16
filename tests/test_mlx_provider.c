/* test_mlx_provider — coverage for the M3 Bridge B MLX stub.
 *
 * Today every chat path returns HU_ERR_NOT_SUPPORTED — these tests pin
 * that contract so a future helpers.c refactor or vtable change can't
 * silently break the dispatcher's fallback assumption.
 *
 * Mirrors the pattern in test_provider_all.c::test_llamacpp_*: prove
 * the provider creates cleanly, vtable methods return NOT_SUPPORTED
 * without dereffing the context, and deinit doesn't leak.
 *
 * When HU_ENABLE_MLX_PROVIDER is defined AND an MLX runtime is wired,
 * a parallel positive-path test file should be added — until then,
 * these stubs ARE the contract.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/mlx.h"
#include "test_framework.h"
#include <string.h>

static void mlx_provider_create_succeeds_with_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_NOT_NULL(p.vtable);
    HU_ASSERT_STR_EQ(p.vtable->get_name(p.ctx), "mlx");
    p.vtable->deinit(p.ctx, &alloc);
}

static void mlx_provider_create_copies_config_strings(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *model = "mlx-community/gemma-4-31b-it-4bit";
    const char *adapter = "/tmp/test.safetensors";
    hu_mlx_config_t cfg = {
        .model_path = model,
        .model_path_len = strlen(model),
        .adapter_path = adapter,
        .adapter_path_len = strlen(adapter),
        .max_tokens = 256,
    };
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    HU_ASSERT_NOT_NULL(p.ctx);
    /* The owned copies aren't observable through the vtable — but
     * deinit must free them without ASan flagging a leak or double
     * free. That's the assertion. */
    p.vtable->deinit(p.ctx, &alloc);
}

static void mlx_provider_create_rejects_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(NULL, NULL, &p), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* The dispatcher safety contract — chat path returns NOT_SUPPORTED, does
 * NOT deref the context. Mirrors the test_provider_all.c daemon-pattern
 * guard the critic flagged on 2026-05-16 as the WRONG kind of test
 * (it covered crash-safety but not fallback behavior). This test
 * directly covers fallback-on-NOT_SUPPORTED at the provider level. */
static void mlx_provider_chat_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);

    hu_chat_request_t req = {0};
    hu_chat_response_t out = {0};
    HU_ASSERT_EQ(p.vtable->chat(p.ctx, &alloc, &req, "model", 5, 0.0, &out), HU_ERR_NOT_SUPPORTED);

    char *legacy_out = NULL;
    size_t legacy_len = 0;
    HU_ASSERT_EQ(p.vtable->chat_with_system(p.ctx, &alloc, "sys", 3, "msg", 3, "model", 5, 0.0,
                                            &legacy_out, &legacy_len),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(legacy_out);
    HU_ASSERT_EQ((long)legacy_len, 0L);

    p.vtable->deinit(p.ctx, &alloc);
}

static void mlx_provider_load_adapter_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    HU_ASSERT_NOT_NULL(p.vtable->load_adapter);
    HU_ASSERT_EQ(p.vtable->load_adapter(p.ctx, &alloc, "/tmp/x.safetensors", 18, "id", 2),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(p.vtable->active_adapter(p.ctx));
    p.vtable->deinit(p.ctx, &alloc);
}

static void mlx_provider_supports_native_tools_is_false(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    HU_ASSERT_FALSE(p.vtable->supports_native_tools(p.ctx));
    p.vtable->deinit(p.ctx, &alloc);
}

void run_mlx_provider_tests(void) {
    HU_TEST_SUITE("mlx_provider");
    HU_RUN_TEST(mlx_provider_create_succeeds_with_defaults);
    HU_RUN_TEST(mlx_provider_create_copies_config_strings);
    HU_RUN_TEST(mlx_provider_create_rejects_null_args);
    HU_RUN_TEST(mlx_provider_chat_returns_not_supported);
    HU_RUN_TEST(mlx_provider_load_adapter_returns_not_supported);
    HU_RUN_TEST(mlx_provider_supports_native_tools_is_false);
}
