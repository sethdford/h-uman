#include "human/core/json.h"
#include "human/tools/image_gen.h"
#include "test_framework.h"
#include <string.h>

static void image_gen_create_registers_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_image_gen_create(&alloc, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "image_generate");
}

static void image_gen_execute_returns_mock_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_image_gen_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json_str = "{\"prompt\":\"a sunset over mountains\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json_str, strlen(json_str), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(result.success);
    HU_ASSERT_NOT_NULL(result.output);
    HU_ASSERT(hu__strcasestr(result.output, "mock") != NULL);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

static void image_gen_url_into_buffer_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char url[512];
    HU_ASSERT_EQ(hu_image_gen_url_into_buffer(&alloc, "a red balloon", 13, url, sizeof(url)),
                 HU_OK);
    HU_ASSERT(hu__strcasestr(url, "mock") != NULL);
}

static void image_gen_missing_prompt_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_image_gen_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json_str = "{\"size\":\"1024x1024\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json_str, strlen(json_str), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(!result.success);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

static void image_gen_download_returns_mock_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    HU_ASSERT_EQ(hu_image_gen_download(&alloc, "a cozy cat", 10, path, sizeof(path)), HU_OK);
    HU_ASSERT(strstr(path, "/tmp/") != NULL || strstr(path, "hu_test") != NULL);
}

static void image_gen_download_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    HU_ASSERT_EQ(hu_image_gen_download(NULL, "x", 1, path, sizeof(path)), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_image_gen_download(&alloc, NULL, 0, path, sizeof(path)),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_image_gen_download(&alloc, "x", 1, path, 2), HU_ERR_INVALID_ARGUMENT);
}

/* DALL·E was shut down 2026-05-12; every request since was a 400 ("Unknown
 * parameter: 'response_format'"). gpt-image-1 takes no response_format,
 * other quality/size values, and answers with base64 only. */
static void image_gen_request_targets_gpt_image_without_response_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *body = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(
        hu_image_gen_build_request(&alloc, "a dog on a beach", 16, "1792x1024", "hd", &body, &len),
        HU_OK);
    hu_json_value_t *j = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, body, len, &j), HU_OK);
    HU_ASSERT_STR_EQ(hu_json_get_string(j, "model"), "gpt-image-1");
    HU_ASSERT_NULL(hu_json_object_get(j, "response_format"));
    HU_ASSERT_STR_EQ(hu_json_get_string(j, "quality"), "high");
    HU_ASSERT_STR_EQ(hu_json_get_string(j, "size"), "1536x1024");
    HU_ASSERT_STR_EQ(hu_json_get_string(j, "prompt"), "a dog on a beach");
    hu_json_free(&alloc, j);
    alloc.free(alloc.ctx, body, len + 1);

    HU_ASSERT_EQ(hu_image_gen_build_request(&alloc, "x", 1, NULL, NULL, &body, &len), HU_OK);
    HU_ASSERT_EQ(hu_json_parse(&alloc, body, len, &j), HU_OK);
    HU_ASSERT_STR_EQ(hu_json_get_string(j, "quality"), "medium");
    HU_ASSERT_STR_EQ(hu_json_get_string(j, "size"), "1024x1024");
    hu_json_free(&alloc, j);
    alloc.free(alloc.ctx, body, len + 1);
}

static void image_gen_parses_the_base64_image(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char ok[] = "{\"data\":[{\"b64_json\":\"aGVsbG8=\"}]}";
    void *bytes = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_image_gen_parse_image(&alloc, ok, strlen(ok), &bytes, &n), HU_OK);
    HU_ASSERT_EQ(n, 5u);
    HU_ASSERT_TRUE(memcmp(bytes, "hello", 5) == 0);
    alloc.free(alloc.ctx, bytes, n);
    static const char url_only[] = "{\"data\":[{\"url\":\"https://example.com/x.png\"}]}";
    HU_ASSERT_EQ(hu_image_gen_parse_image(&alloc, url_only, strlen(url_only), &bytes, &n),
                 HU_ERR_IO);
    HU_ASSERT_NULL(bytes);
}

void run_image_gen_tests(void) {
    HU_RUN_TEST(image_gen_request_targets_gpt_image_without_response_format);
    HU_RUN_TEST(image_gen_parses_the_base64_image);
    HU_TEST_SUITE("ImageGen");
    HU_RUN_TEST(image_gen_create_registers_name);
    HU_RUN_TEST(image_gen_execute_returns_mock_url);
    HU_RUN_TEST(image_gen_url_into_buffer_mock);
    HU_RUN_TEST(image_gen_missing_prompt_fails);
    HU_RUN_TEST(image_gen_download_returns_mock_path);
    HU_RUN_TEST(image_gen_download_null_args);
}
