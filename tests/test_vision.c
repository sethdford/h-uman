/* Vision module tests — image reading, context building, describe flow. */

#include "human/context/vision.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "test_framework.h"
#include <string.h>

/* ── hu_vision_build_context ───────────────────────────────────────────── */

static void vision_build_context_with_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *desc = "a sunset over the ocean";
    size_t desc_len = strlen(desc);
    size_t out_len = 0;
    char *ctx = hu_vision_build_context(&alloc, desc, desc_len, &out_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "sunset") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "Image Context") != NULL);
    alloc.free(alloc.ctx, ctx, out_len + 1);
}

static void vision_build_context_null_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 99;
    char *ctx = hu_vision_build_context(&alloc, NULL, 0, &out_len);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(out_len, 0u);
}

static void vision_build_context_empty_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 99;
    char *ctx = hu_vision_build_context(&alloc, "", 0, &out_len);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(out_len, 0u);
}

/* ── hu_vision_read_image ───────────────────────────────────────────────── */

static void vision_read_image_null_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *base64 = NULL;
    size_t base64_len = 0;
    char *media_type = NULL;
    size_t media_type_len = 0;
    hu_error_t err = hu_vision_read_image(&alloc, NULL, 0, &base64, &base64_len,
                                          &media_type, &media_type_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(base64);
    HU_ASSERT_NULL(media_type);
}

static void vision_read_image_mock_in_test(void) {
    /* In HU_IS_TEST, hu_vision_read_image returns mock base64 without file I/O */
    hu_allocator_t alloc = hu_system_allocator();
    char *base64 = NULL;
    size_t base64_len = 0;
    char *media_type = NULL;
    size_t media_type_len = 0;
    hu_error_t err = hu_vision_read_image(&alloc, "/nonexistent/path.png", 20,
                                          &base64, &base64_len,
                                          &media_type, &media_type_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(base64);
    HU_ASSERT_TRUE(base64_len > 0);
    HU_ASSERT_NOT_NULL(media_type);
    HU_ASSERT_STR_EQ(media_type, "image/png");
    alloc.free(alloc.ctx, base64, base64_len + 1);
    alloc.free(alloc.ctx, media_type, media_type_len + 1);
}

/* ── hu_vision_describe_image (no vision support) ───────────────────────── */

static bool no_vision_supports_vision(void *ctx) {
    (void)ctx;
    return false;
}

static const char *no_vision_get_name(void *ctx) {
    (void)ctx;
    return "no_vision";
}

static hu_provider_vtable_t no_vision_vtable = {
    .chat_with_system = NULL,
    .chat = NULL,
    .supports_native_tools = NULL,
    .get_name = no_vision_get_name,
    .deinit = NULL,
    .supports_vision = no_vision_supports_vision,
};

static void vision_describe_image_no_vision_support(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t provider = {
        .ctx = NULL,
        .vtable = &no_vision_vtable,
    };
    char *desc = NULL;
    size_t desc_len = 0;
    hu_error_t err = hu_vision_describe_image(
        &alloc, &provider, "/tmp/test.png", 11, "gpt-4o", 6, &desc, &desc_len);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(desc);
}

/* Provider with supports_vision = NULL (optional field) */
static hu_provider_vtable_t no_vision_vtable_null = {
    .chat_with_system = NULL,
    .chat = NULL,
    .supports_native_tools = NULL,
    .get_name = no_vision_get_name,
    .deinit = NULL,
    .supports_vision = NULL,
};

static void vision_describe_image_no_vision_vtable_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t provider = {
        .ctx = NULL,
        .vtable = &no_vision_vtable_null,
    };
    char *desc = NULL;
    size_t desc_len = 0;
    hu_error_t err = hu_vision_describe_image(
        &alloc, &provider, "/tmp/test.png", 11, "gpt-4o", 6, &desc, &desc_len);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(desc);
}

static void vision_describe_image_null_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t provider = {.ctx = NULL, .vtable = NULL};
    char *desc = NULL;
    size_t desc_len = 0;
    hu_error_t err = hu_vision_describe_image(&alloc, &provider, "test.png", 8, "gpt-4o", 6,
                                              &desc, &desc_len);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(desc);
}

static void vision_describe_image_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t provider = {.ctx = NULL, .vtable = &no_vision_vtable};
    char *desc = NULL;
    size_t desc_len = 0;

    HU_ASSERT_EQ(hu_vision_describe_image(NULL, &provider, "test.png", 8, "gpt-4o", 6,
                                          &desc, &desc_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_vision_describe_image(&alloc, NULL, "test.png", 8, "gpt-4o", 6,
                                          &desc, &desc_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_vision_describe_image(&alloc, &provider, "test.png", 8, "gpt-4o", 6,
                                          NULL, &desc_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_vision_describe_image(&alloc, &provider, "test.png", 8, "gpt-4o", 6,
                                          &desc, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void vision_build_context_null_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 99;

    char *ctx = hu_vision_build_context(NULL, "desc", 4, &out_len);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(out_len, 0u);

    out_len = 99;
    ctx = hu_vision_build_context(&alloc, NULL, 0, &out_len);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(out_len, 0u);
}

/* Mock provider with vision support for HU_IS_TEST describe flow */
static bool mock_vision_supports_vision(void *ctx) {
    (void)ctx;
    return true;
}

static const char *mock_vision_get_name(void *ctx) {
    (void)ctx;
    return "mock_vision";
}

static hu_error_t mock_vision_chat(void *ctx, hu_allocator_t *alloc,
                                  const hu_chat_request_t *request, const char *model,
                                  size_t model_len, double temperature,
                                  hu_chat_response_t *out) {
    (void)ctx;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    const char *resp = "A blue sky with white clouds.";
    out->content = hu_strndup(alloc, resp, strlen(resp));
    out->content_len = out->content ? strlen(resp) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->usage.prompt_tokens = 1;
    out->usage.completion_tokens = 2;
    out->usage.total_tokens = 3;
    out->model = NULL;
    out->model_len = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static const hu_provider_vtable_t mock_vision_vtable = {
    .chat_with_system = NULL,
    .chat = mock_vision_chat,
    .supports_native_tools = NULL,
    .get_name = mock_vision_get_name,
    .deinit = NULL,
    .supports_vision = mock_vision_supports_vision,
};

static void vision_describe_image_test_mock_returns_description(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t provider = {.ctx = NULL, .vtable = &mock_vision_vtable};
    char *desc = NULL;
    size_t desc_len = 0;

    hu_error_t err = hu_vision_describe_image(&alloc, &provider, "/tmp/test.png", 12,
                                              "gpt-4o", 6, &desc, &desc_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(desc);
    HU_ASSERT_TRUE(desc_len > 0);
    HU_ASSERT_TRUE(strstr(desc, "sky") != NULL || strstr(desc, "clouds") != NULL);

    alloc.free(alloc.ctx, desc, desc_len + 1);
#endif
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

void run_vision_tests(void) {
    HU_TEST_SUITE("vision");
    HU_RUN_TEST(vision_build_context_with_description);
    HU_RUN_TEST(vision_build_context_null_description);
    HU_RUN_TEST(vision_build_context_empty_description);
    HU_RUN_TEST(vision_build_context_null_returns_null);
    HU_RUN_TEST(vision_read_image_null_path);
    HU_RUN_TEST(vision_read_image_mock_in_test);
    HU_RUN_TEST(vision_describe_image_null_args_returns_error);
    HU_RUN_TEST(vision_describe_image_no_vision_support);
    HU_RUN_TEST(vision_describe_image_no_vision_vtable_null);
    HU_RUN_TEST(vision_describe_image_null_vtable);
    HU_RUN_TEST(vision_describe_image_test_mock_returns_description);
}
