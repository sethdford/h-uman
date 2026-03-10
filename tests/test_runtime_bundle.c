#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/runtime_bundle.h"
#include "test_framework.h"
#include <string.h>

/* Mock provider for testing */
static const char *mock_get_name(void *ctx) {
    (void)ctx;
    return "mock";
}

static void mock_deinit(void *ctx, hu_allocator_t *a) {
    (void)ctx;
    (void)a;
}

static hu_error_t mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    (void)ctx;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)alloc;
    memset(out, 0, sizeof(*out));
    return HU_OK;
}

static hu_error_t mock_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *system_prompt,
                                        size_t system_prompt_len, const char *message,
                                        size_t message_len, const char *model, size_t model_len,
                                        double temperature, char **out, size_t *out_len) {
    (void)ctx;
    (void)alloc;
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    *out = NULL;
    *out_len = 0;
    return HU_OK;
}

static bool mock_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static const hu_provider_vtable_t mock_vtable = {
    .chat_with_system = mock_chat_with_system,
    .chat = mock_chat,
    .supports_native_tools = mock_supports_native_tools,
    .get_name = mock_get_name,
    .deinit = mock_deinit,
};

static hu_provider_t make_mock_provider(void) {
    return (hu_provider_t){.ctx = NULL, .vtable = &mock_vtable};
}

/* ─────────────────────────────────────────────────────────────────────────
 * Tests
 * ───────────────────────────────────────────────────────────────────────── */

static void test_init_null_alloc_returns_invalid_argument(void) {
    hu_provider_t primary = make_mock_provider();
    hu_runtime_bundle_t bundle = {0};
    hu_error_t err = hu_runtime_bundle_init(NULL, primary, 0, 0, &bundle);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_init_null_out_returns_invalid_argument(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t primary = make_mock_provider();
    hu_error_t err = hu_runtime_bundle_init(&alloc, primary, 0, 0, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_init_no_wrapping_stores_primary_directly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t primary = make_mock_provider();
    hu_runtime_bundle_t bundle = {0};

    hu_error_t err = hu_runtime_bundle_init(&alloc, primary, 0, 0, &bundle);
    HU_ASSERT_EQ(err, HU_OK);

    hu_provider_t got = hu_runtime_bundle_provider(&bundle);
    HU_ASSERT_EQ(got.ctx, primary.ctx);
    HU_ASSERT_EQ(got.vtable, primary.vtable);
    HU_ASSERT_STR_EQ(got.vtable->get_name(got.ctx), "mock");

    hu_runtime_bundle_deinit(&bundle, &alloc);
}

static void test_init_with_retries_wraps_with_reliable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t primary = make_mock_provider();
    hu_runtime_bundle_t bundle = {0};

    hu_error_t err = hu_runtime_bundle_init(&alloc, primary, 2, 50, &bundle);
    HU_ASSERT_EQ(err, HU_OK);

    hu_provider_t got = hu_runtime_bundle_provider(&bundle);
    HU_ASSERT_NOT_NULL(got.ctx);
    HU_ASSERT_NOT_NULL(got.vtable);
    HU_ASSERT_STR_EQ(got.vtable->get_name(got.ctx), "mock");

    hu_runtime_bundle_deinit(&bundle, &alloc);
}

static void test_provider_accessor_returns_bundle_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t primary = make_mock_provider();
    hu_runtime_bundle_t bundle = {0};

    hu_runtime_bundle_init(&alloc, primary, 0, 0, &bundle);
    hu_provider_t got = hu_runtime_bundle_provider(&bundle);
    HU_ASSERT_EQ(got.vtable, &mock_vtable);
    HU_ASSERT_STR_EQ(got.vtable->get_name(got.ctx), "mock");

    hu_runtime_bundle_deinit(&bundle, &alloc);
}

static void test_provider_accessor_null_bundle_returns_zeroed(void) {
    hu_provider_t got = hu_runtime_bundle_provider(NULL);
    HU_ASSERT_NULL(got.ctx);
    HU_ASSERT_NULL(got.vtable);
}

static void test_deinit_null_no_ops(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_runtime_bundle_deinit(NULL, &alloc);
    /* No crash */
}

static void test_deinit_calls_vtable_and_zeroes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t primary = make_mock_provider();
    hu_runtime_bundle_t bundle = {0};

    hu_runtime_bundle_init(&alloc, primary, 0, 0, &bundle);
    hu_runtime_bundle_deinit(&bundle, &alloc);

    HU_ASSERT_NULL(bundle.provider.ctx);
    HU_ASSERT_NULL(bundle.provider.vtable);
    HU_ASSERT_NULL(bundle.inner_ctx);
    HU_ASSERT_NULL(bundle.inner_vtable);
}

static void test_deinit_wrapped_provider_cleans_up(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t primary = make_mock_provider();
    hu_runtime_bundle_t bundle = {0};

    hu_runtime_bundle_init(&alloc, primary, 1, 50, &bundle);
    hu_runtime_bundle_deinit(&bundle, &alloc);

    HU_ASSERT_NULL(bundle.provider.ctx);
    HU_ASSERT_NULL(bundle.provider.vtable);
}

void run_runtime_bundle_tests(void) {
    HU_TEST_SUITE("runtime_bundle");

    HU_RUN_TEST(test_init_null_alloc_returns_invalid_argument);
    HU_RUN_TEST(test_init_null_out_returns_invalid_argument);
    HU_RUN_TEST(test_init_no_wrapping_stores_primary_directly);
    HU_RUN_TEST(test_init_with_retries_wraps_with_reliable);
    HU_RUN_TEST(test_provider_accessor_returns_bundle_provider);
    HU_RUN_TEST(test_provider_accessor_null_bundle_returns_zeroed);
    HU_RUN_TEST(test_deinit_null_no_ops);
    HU_RUN_TEST(test_deinit_calls_vtable_and_zeroes);
    HU_RUN_TEST(test_deinit_wrapped_provider_cleans_up);
}
