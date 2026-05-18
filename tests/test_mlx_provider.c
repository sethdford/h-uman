/* test_mlx_provider — coverage for the M3 Bridge B MLX provider.
 *
 * Two contracts pinned here, both required for the daemon's fallback:
 *
 *   1. Unlinked build (HU_ENABLE_MLX_PROVIDER undefined — the default):
 *      every chat path returns HU_ERR_NOT_SUPPORTED so the agent's
 *      provider fallback fires cleanly. The existing tests cover this.
 *
 *   2. Linked build (HU_ENABLE_MLX_PROVIDER defined + Apple Silicon):
 *      chat path INVOKES the subprocess helper (`mlx_run_subprocess`)
 *      modeled after run_claude_cli in src/providers/claude_cli.c.
 *      Slice 1 (Phase B1 first slice, 2026-05-17) adds the helper
 *      but its end-to-end test that spawns python3 -m mlx_lm.generate
 *      lands in slice 2 with the test-fixture shim. For slice 1, the
 *      compile-time gates are pinned via the new structural test below.
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
    HU_ASSERT_NOT_NULL(p.vtable->unload_adapter); /* CodeRabbit 2026-05-17 — pin
                                                     the third member of the
                                                     adapter triple too. */
    HU_ASSERT_EQ(p.vtable->load_adapter(p.ctx, &alloc, "/tmp/x.safetensors", 18, "id", 2),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(p.vtable->unload_adapter(p.ctx, "id", 2), HU_ERR_NOT_SUPPORTED);
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

/* Slice 1 structural pin (B1 — 2026-05-17): the chat path MUST flow
 * through the subprocess helper when the gates are met. We can't
 * directly observe "the helper got called" from outside, but we CAN
 * assert that the helper's compile-time guard logic preserves the
 * NOT_SUPPORTED contract in the unlinked build — which is what the
 * existing chat_returns_not_supported test does.
 *
 * This test additionally pins that the request->user-message flattening
 * does NOT touch the out parameter on the NOT_SUPPORTED path (a
 * regression we'd otherwise only catch by inspecting code). When
 * mlx_run_subprocess returns NOT_SUPPORTED before any allocation, the
 * caller's `out` struct must be untouched so the daemon's fallback path
 * can safely retry with another provider. */
static void mlx_provider_chat_does_not_mutate_out_on_unsupported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);

    hu_chat_message_t msgs[1] = {{
        .role = HU_ROLE_USER,
        .content = "hello",
        .content_len = 5,
    }};
    hu_chat_request_t req = {.messages = msgs, .messages_count = 1};

    /* Pre-fill out with sentinel values; require they survive a
     * NOT_SUPPORTED return — caller still owns the struct. */
    hu_chat_response_t out = {0};
    out.content = (const char *)0xdeadbeef;
    out.content_len = 0x4242;
    HU_ASSERT_EQ(p.vtable->chat(p.ctx, &alloc, &req, "model", 5, 0.0, &out), HU_ERR_NOT_SUPPORTED);
    /* Sentinel preserved — helper must not partial-fill on the
     * NOT_SUPPORTED path. */
    HU_ASSERT_EQ((unsigned long)(uintptr_t)out.content, (unsigned long)0xdeadbeef);
    HU_ASSERT_EQ((unsigned long)out.content_len, (unsigned long)0x4242);

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
    HU_RUN_TEST(mlx_provider_chat_does_not_mutate_out_on_unsupported);
}
