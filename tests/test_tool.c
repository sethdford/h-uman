#include "test_framework.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/shell.h"
#include "seaclaw/tools/factory.h"
#include "seaclaw/tools/validation.h"
#include <string.h>

static void test_shell_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "shell");
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_shell_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_shell_create(&alloc, NULL, 0, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "shell");
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_shell_description(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_shell_create(&alloc, NULL, 0, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);
    const char *desc = tool.vtable->description(tool.ctx);
    SC_ASSERT_NOT_NULL(desc);
    SC_ASSERT_TRUE(strlen(desc) > 0);
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_shell_parameters_json(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_shell_create(&alloc, NULL, 0, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(params);
    SC_ASSERT_TRUE(strstr(params, "command") != NULL);
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_shell_execute_disabled_in_test(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_shell_create(&alloc, NULL, 0, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    sc_json_value_t *args = sc_json_object_new(&alloc);
    SC_ASSERT_NOT_NULL(args);
    sc_json_value_t *cmd_val = sc_json_string_new(&alloc, "echo hello", 10);
    sc_json_object_set(&alloc, args, "command", cmd_val);

    sc_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    sc_json_free(&alloc, args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.success);
    SC_ASSERT_NOT_NULL(result.output);
    if (result.output) {
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    }
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_validate_path_rejects_traversal(void) {
    SC_ASSERT_EQ(sc_tool_validate_path("../etc/passwd", NULL, 0), SC_ERR_TOOL_VALIDATION);
    SC_ASSERT_EQ(sc_tool_validate_path("foo/../bar", NULL, 0), SC_ERR_TOOL_VALIDATION);
    SC_ASSERT_EQ(sc_tool_validate_path("/etc/../etc/passwd", NULL, 0), SC_ERR_TOOL_VALIDATION);
    SC_ASSERT_EQ(sc_tool_validate_path("..", NULL, 0), SC_ERR_TOOL_VALIDATION);
}

static void test_validate_path_accepts_safe(void) {
    SC_ASSERT_EQ(sc_tool_validate_path("foo.txt", NULL, 0), SC_OK);
    SC_ASSERT_EQ(sc_tool_validate_path("subdir/file.txt", NULL, 0), SC_OK);
    {
        const char *ws = "/tmp/workspace";
        SC_ASSERT_EQ(sc_tool_validate_path("/tmp/workspace/file", ws, strlen(ws)), SC_OK);
    }
}

static void test_validate_path_rejects_outside_workspace(void) {
    SC_ASSERT_EQ(sc_tool_validate_path("/etc/passwd", "/tmp/workspace", 15), SC_ERR_TOOL_VALIDATION);
    SC_ASSERT_EQ(sc_tool_validate_path("/tmp/workspace_evil", "/tmp/workspace", 15), SC_ERR_TOOL_VALIDATION);
}

static void test_validate_url_https_only(void) {
    SC_ASSERT_EQ(sc_tool_validate_url("http://example.com"), SC_ERR_TOOL_VALIDATION);
    SC_ASSERT_EQ(sc_tool_validate_url("https://example.com"), SC_OK);
}

static void test_validate_url_rejects_private_ips(void) {
    SC_ASSERT_EQ(sc_tool_validate_url("https://127.0.0.1/"), SC_ERR_TOOL_VALIDATION);
    SC_ASSERT_EQ(sc_tool_validate_url("https://10.0.0.1/"), SC_ERR_TOOL_VALIDATION);
    SC_ASSERT_EQ(sc_tool_validate_url("https://192.168.1.1/"), SC_ERR_TOOL_VALIDATION);
    SC_ASSERT_EQ(sc_tool_validate_url("https://[::1]/"), SC_ERR_TOOL_VALIDATION);
}

static void test_tools_factory_create_default(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t *tools = NULL;
    size_t count = 0;
    sc_error_t err = sc_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, &tools, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(tools);
    SC_ASSERT(count >= 1);
    SC_ASSERT_STR_EQ(tools[0].vtable->name(tools[0].ctx), "shell");
    sc_tools_destroy_default(&alloc, tools, count);
}

void run_tool_tests(void) {
    SC_TEST_SUITE("Tool");
    SC_RUN_TEST(test_validate_path_rejects_traversal);
    SC_RUN_TEST(test_validate_path_accepts_safe);
    SC_RUN_TEST(test_validate_path_rejects_outside_workspace);
    SC_RUN_TEST(test_validate_url_https_only);
    SC_RUN_TEST(test_validate_url_rejects_private_ips);
    SC_RUN_TEST(test_shell_create_succeeds);
    SC_RUN_TEST(test_shell_name);
    SC_RUN_TEST(test_shell_description);
    SC_RUN_TEST(test_shell_parameters_json);
    SC_RUN_TEST(test_shell_execute_disabled_in_test);
    SC_RUN_TEST(test_tools_factory_create_default);
}
