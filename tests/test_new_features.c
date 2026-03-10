/* Tests for PDF tool, health endpoints, config reload, ws_streaming. */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/gateway.h"
#include "human/tool.h"
#include "human/tools/pdf.h"
#include "test_framework.h"
#include <string.h>

static void test_pdf_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_pdf_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "pdf");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_pdf_missing_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_pdf_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    hu_json_free(&alloc, args);
    HU_ASSERT_FALSE(result.success);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_pdf_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_pdf_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "/tmp/test.pdf", 13));
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    hu_json_free(&alloc, args);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(result.output);
    HU_ASSERT_TRUE(strstr(result.output, "test.pdf") != NULL);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_pdf_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_pdf_create(&alloc, &tool);
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, NULL, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_health_endpoints_exist(void) {
    HU_ASSERT_TRUE(hu_gateway_path_is("/health", "/health"));
    HU_ASSERT_TRUE(hu_gateway_path_is("/healthz", "/healthz"));
    HU_ASSERT_TRUE(hu_gateway_path_is("/ready", "/ready"));
    HU_ASSERT_TRUE(hu_gateway_path_is("/readyz", "/readyz"));
    HU_ASSERT_TRUE(hu_gateway_path_is("/health/", "/health"));
    HU_ASSERT_FALSE(hu_gateway_path_is("/healthcheck", "/health"));
    HU_ASSERT_FALSE(hu_gateway_path_is(NULL, "/health"));
    HU_ASSERT_FALSE(hu_gateway_path_is("/health", NULL));
}

static void test_config_reload_flag(void) {
    HU_ASSERT_FALSE(hu_config_get_and_clear_reload_requested());
    hu_config_set_reload_requested();
    HU_ASSERT_TRUE(hu_config_get_and_clear_reload_requested());
    HU_ASSERT_FALSE(hu_config_get_and_clear_reload_requested());
}

static void test_ws_streaming_config(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_arena_t *arena = hu_arena_create(backing);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    HU_ASSERT_FALSE(hu_config_get_provider_ws_streaming(&cfg, "openai"));
    HU_ASSERT_FALSE(hu_config_get_provider_ws_streaming(NULL, "openai"));
    const char *json =
        "{\"providers\":[{\"name\":\"openai\"},{\"name\":\"test\",\"ws_streaming\":true}]}";
    hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_FALSE(hu_config_get_provider_ws_streaming(&cfg, "openai"));
    HU_ASSERT_TRUE(hu_config_get_provider_ws_streaming(&cfg, "test"));
    hu_config_deinit(&cfg);
}

static void test_suppress_tool_progress(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_arena_t *arena = hu_arena_create(backing);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    HU_ASSERT_FALSE(cfg.channels.suppress_tool_progress);
    const char *json = "{\"channels\":{\"suppress_tool_progress\":true}}";
    hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_TRUE(cfg.channels.suppress_tool_progress);
    hu_config_deinit(&cfg);
}

void run_new_features_tests(void) {
    HU_TEST_SUITE("New Features - PDF");
    HU_RUN_TEST(test_pdf_create);
    HU_RUN_TEST(test_pdf_missing_path);
    HU_RUN_TEST(test_pdf_test_mode);
    HU_RUN_TEST(test_pdf_null_args);
    HU_TEST_SUITE("New Features - Health");
    HU_RUN_TEST(test_health_endpoints_exist);
    HU_TEST_SUITE("New Features - Config");
    HU_RUN_TEST(test_config_reload_flag);
    HU_RUN_TEST(test_ws_streaming_config);
    HU_RUN_TEST(test_suppress_tool_progress);
}
