#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/tool.h"
#include "human/tools/factory.h"
#include "human/tools/shell.h"
#include "human/tools/validation.h"
#include "test_framework.h"
#include <string.h>

static void test_shell_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "shell");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_shell_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_shell_create(&alloc, NULL, 0, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "shell");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_shell_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_shell_create(&alloc, NULL, 0, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    const char *desc = tool.vtable->description(tool.ctx);
    HU_ASSERT_NOT_NULL(desc);
    HU_ASSERT_TRUE(strlen(desc) > 0);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_shell_parameters_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_shell_create(&alloc, NULL, 0, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(params);
    HU_ASSERT_TRUE(strstr(params, "command") != NULL);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_shell_execute_disabled_in_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_shell_create(&alloc, NULL, 0, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    hu_json_value_t *args = hu_json_object_new(&alloc);
    HU_ASSERT_NOT_NULL(args);
    hu_json_value_t *cmd_val = hu_json_string_new(&alloc, "echo hello", 10);
    hu_json_object_set(&alloc, args, "command", cmd_val);

    hu_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(result.output);
    if (result.output) {
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    }
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_validate_path_rejects_traversal(void) {
    HU_ASSERT_EQ(hu_tool_validate_path("../etc/passwd", NULL, 0), HU_ERR_TOOL_VALIDATION);
    HU_ASSERT_EQ(hu_tool_validate_path("foo/../bar", NULL, 0), HU_ERR_TOOL_VALIDATION);
    HU_ASSERT_EQ(hu_tool_validate_path("/etc/../etc/passwd", NULL, 0), HU_ERR_TOOL_VALIDATION);
    HU_ASSERT_EQ(hu_tool_validate_path("..", NULL, 0), HU_ERR_TOOL_VALIDATION);
}

static void test_validate_path_accepts_safe(void) {
    HU_ASSERT_EQ(hu_tool_validate_path("foo.txt", NULL, 0), HU_OK);
    HU_ASSERT_EQ(hu_tool_validate_path("subdir/file.txt", NULL, 0), HU_OK);
    {
        const char *ws = "/tmp/workspace";
        HU_ASSERT_EQ(hu_tool_validate_path("/tmp/workspace/file", ws, strlen(ws)), HU_OK);
    }
}

static void test_validate_path_rejects_outside_workspace(void) {
    HU_ASSERT_EQ(hu_tool_validate_path("/etc/passwd", "/tmp/workspace", 15),
                 HU_ERR_TOOL_VALIDATION);
    HU_ASSERT_EQ(hu_tool_validate_path("/tmp/workspace_evil", "/tmp/workspace", 15),
                 HU_ERR_TOOL_VALIDATION);
}

static void test_validate_url_https_only(void) {
    HU_ASSERT_EQ(hu_tool_validate_url("http://example.com"), HU_ERR_TOOL_VALIDATION);
    HU_ASSERT_EQ(hu_tool_validate_url("https://example.com"), HU_OK);
}

static void test_validate_url_rejects_private_ips(void) {
    HU_ASSERT_EQ(hu_tool_validate_url("https://127.0.0.1/"), HU_ERR_TOOL_VALIDATION);
    HU_ASSERT_EQ(hu_tool_validate_url("https://10.0.0.1/"), HU_ERR_TOOL_VALIDATION);
    HU_ASSERT_EQ(hu_tool_validate_url("https://192.168.1.1/"), HU_ERR_TOOL_VALIDATION);
    HU_ASSERT_EQ(hu_tool_validate_url("https://[::1]/"), HU_ERR_TOOL_VALIDATION);
}

static void test_tools_factory_create_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tools);
    HU_ASSERT(count >= 1);
    HU_ASSERT_STR_EQ(tools[0].vtable->name(tools[0].ctx), "shell");
    hu_tools_destroy_default(&alloc, tools, count);
}

void run_tool_tests(void) {
    HU_TEST_SUITE("Tool");
    HU_RUN_TEST(test_validate_path_rejects_traversal);
    HU_RUN_TEST(test_validate_path_accepts_safe);
    HU_RUN_TEST(test_validate_path_rejects_outside_workspace);
    HU_RUN_TEST(test_validate_url_https_only);
    HU_RUN_TEST(test_validate_url_rejects_private_ips);
    HU_RUN_TEST(test_shell_create_succeeds);
    HU_RUN_TEST(test_shell_name);
    HU_RUN_TEST(test_shell_description);
    HU_RUN_TEST(test_shell_parameters_json);
    HU_RUN_TEST(test_shell_execute_disabled_in_test);
    HU_RUN_TEST(test_tools_factory_create_default);
}
