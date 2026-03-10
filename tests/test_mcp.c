#include "human/core/allocator.h"
#include "human/mcp.h"
#include "human/mcp_server.h"
#include "human/memory.h"
#include "human/tool.h"
#include "test_framework.h"
#include <string.h>

static void test_mcp_server_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, &cfg);
    HU_ASSERT_NOT_NULL(srv);
    hu_mcp_server_destroy(srv);
}

static void test_mcp_server_connect_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, &cfg);
    HU_ASSERT_NOT_NULL(srv);
    hu_error_t err = hu_mcp_server_connect(srv);
    HU_ASSERT_EQ(err, HU_OK);
    hu_mcp_server_destroy(srv);
}

static void test_mcp_server_list_tools_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, &cfg);
    hu_mcp_server_connect(srv);
    char **names = NULL, **descs = NULL, **params = NULL;
    size_t count = 0;
    hu_error_t err = hu_mcp_server_list_tools(srv, &alloc, &names, &descs, &params, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(names[0], "mock_tool");
    HU_ASSERT_NOT_NULL(params[0]);
    HU_ASSERT_TRUE(strlen(params[0]) > 2);
    for (size_t i = 0; i < count; i++) {
        alloc.free(alloc.ctx, names[i], strlen(names[i]) + 1);
        alloc.free(alloc.ctx, descs[i], strlen(descs[i]) + 1);
        if (params[i])
            alloc.free(alloc.ctx, params[i], strlen(params[i]) + 1);
    }
    alloc.free(alloc.ctx, names, count * sizeof(char *));
    alloc.free(alloc.ctx, descs, count * sizeof(char *));
    alloc.free(alloc.ctx, params, count * sizeof(char *));
    hu_mcp_server_destroy(srv);
}

static void test_mcp_server_call_tool_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, &cfg);
    hu_mcp_server_connect(srv);
    char *result = NULL;
    size_t result_len = 0;
    hu_error_t err = hu_mcp_server_call_tool(srv, &alloc, "mock_tool", "{}", &result, &result_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, result_len + 1);
    hu_mcp_server_destroy(srv);
}

static void test_mcp_init_tools_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err = hu_mcp_init_tools(&alloc, &cfg, 1, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    hu_mcp_free_tools(&alloc, tools, count);
}

/* ── MCP Host (server mode) tests ────────────────────────────────────────── */

static hu_error_t mock_tool_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    (void)ctx;
    (void)args;
    const char *msg = "mock ok";
    size_t len = strlen(msg);
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, msg, len + 1);
    *out = hu_tool_result_ok_owned(buf, len);
    return HU_OK;
}
static const char *mock_tool_name(void *ctx) {
    (void)ctx;
    return "test_tool";
}
static const char *mock_tool_desc(void *ctx) {
    (void)ctx;
    return "A test tool";
}
static const char *mock_tool_params(void *ctx) {
    (void)ctx;
    return "{\"type\":\"object\",\"properties\":{}}";
}

static const hu_tool_vtable_t mock_host_vtable = {
    .execute = mock_tool_execute,
    .name = mock_tool_name,
    .description = mock_tool_desc,
    .parameters_json = mock_tool_params,
    .deinit = NULL,
};

static void test_mcp_host_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[1];
    tools[0].ctx = NULL;
    tools[0].vtable = &mock_host_vtable;

    hu_mcp_host_t *host = NULL;
    hu_error_t err = hu_mcp_host_create(&alloc, tools, 1, NULL, &host);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(host);
    hu_mcp_host_destroy(host);
}

static void test_mcp_host_create_null_alloc(void) {
    hu_mcp_host_t *host = NULL;
    hu_error_t err = hu_mcp_host_create(NULL, NULL, 0, NULL, &host);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_mcp_host_create_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_mcp_host_create(&alloc, NULL, 0, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_mcp_host_create_zero_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_host_t *host = NULL;
    hu_error_t err = hu_mcp_host_create(&alloc, NULL, 0, NULL, &host);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(host);
    hu_mcp_host_destroy(host);
}

static void test_mcp_host_run_null(void) {
    hu_error_t err = hu_mcp_host_run(NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_mcp_host_destroy_null(void) {
    hu_mcp_host_destroy(NULL); /* should not crash */
}

static void test_mcp_host_create_with_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_tool_t tools[1];
    tools[0].ctx = NULL;
    tools[0].vtable = &mock_host_vtable;
    hu_mcp_host_t *host = NULL;
    hu_error_t err = hu_mcp_host_create(&alloc, tools, 1, &mem, &host);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(host);
    hu_mcp_host_destroy(host);
    if (mem.vtable && mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void test_mcp_server_null_config_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, NULL);
    HU_ASSERT_NULL(srv);
}

static void test_mcp_server_call_tool_nonexistent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, &cfg);
    HU_ASSERT_NOT_NULL(srv);
    hu_mcp_server_connect(srv);
    char *result = NULL;
    size_t result_len = 0;
    hu_error_t err =
        hu_mcp_server_call_tool(srv, &alloc, "nonexistent_tool", "{}", &result, &result_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, result_len + 1);
    hu_mcp_server_destroy(srv);
}

static void test_mcp_server_double_connect(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, &cfg);
    HU_ASSERT_NOT_NULL(srv);
    hu_error_t err1 = hu_mcp_server_connect(srv);
    hu_error_t err2 = hu_mcp_server_connect(srv);
    HU_ASSERT_EQ(err1, HU_OK);
    HU_ASSERT_EQ(err2, HU_OK);
    hu_mcp_server_destroy(srv);
}

static void test_mcp_init_tools_null_alloc(void) {
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err = hu_mcp_init_tools(NULL, NULL, 0, &tools, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_mcp_init_tools_zero_configs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err = hu_mcp_init_tools(&alloc, NULL, 0, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(tools);
    HU_ASSERT_EQ(count, 0u);
}

static void test_mcp_host_create_many_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const hu_tool_vtable_t vt = {
        .execute = mock_tool_execute,
        .name = mock_tool_name,
        .description = mock_tool_desc,
        .parameters_json = mock_tool_params,
        .deinit = NULL,
    };
    hu_tool_t tools[6];
    for (size_t i = 0; i < 6; i++) {
        tools[i].ctx = NULL;
        tools[i].vtable = &vt;
    }
    hu_mcp_host_t *host = NULL;
    hu_error_t err = hu_mcp_host_create(&alloc, tools, 6, NULL, &host);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(host);
    hu_mcp_host_destroy(host);
}

static void test_mcp_server_list_tools_not_connected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, &cfg);
    HU_ASSERT_NOT_NULL(srv);
    char **names = NULL, **descs = NULL, **params = NULL;
    size_t count = 0;
    hu_error_t err = hu_mcp_server_list_tools(srv, &alloc, &names, &descs, &params, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    for (size_t i = 0; i < count; i++) {
        alloc.free(alloc.ctx, names[i], strlen(names[i]) + 1);
        alloc.free(alloc.ctx, descs[i], strlen(descs[i]) + 1);
        if (params && params[i])
            alloc.free(alloc.ctx, params[i], strlen(params[i]) + 1);
    }
    alloc.free(alloc.ctx, names, count * sizeof(char *));
    alloc.free(alloc.ctx, descs, count * sizeof(char *));
    if (params)
        alloc.free(alloc.ctx, params, count * sizeof(char *));
    hu_mcp_server_destroy(srv);
}

static void test_mcp_init_tools_null_out_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err = hu_mcp_init_tools(&alloc, &cfg, 1, NULL, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    err = hu_mcp_init_tools(&alloc, &cfg, 1, &tools, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_mcp_host_create_null_alloc_rejected(void) {
    hu_tool_t tools[1] = {{.ctx = NULL, .vtable = &mock_host_vtable}};
    hu_mcp_host_t *host = NULL;
    hu_error_t err = hu_mcp_host_create(NULL, tools, 1, NULL, &host);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_mcp_server_create_null_command_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = NULL, .args = NULL, .args_count = 0};
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, &cfg);
    HU_ASSERT_NULL(srv);
}

static void test_mcp_server_create_null_alloc_fails(void) {
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_mcp_server_t *srv = hu_mcp_server_create(NULL, &cfg);
    HU_ASSERT_NULL(srv);
}

static void test_mcp_free_tools_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_free_tools(&alloc, NULL, 0);
}

void run_mcp_tests(void) {
    HU_TEST_SUITE("MCP Client");
    HU_RUN_TEST(test_mcp_server_create_destroy);
    HU_RUN_TEST(test_mcp_server_connect_test_mode);
    HU_RUN_TEST(test_mcp_server_list_tools_mock);
    HU_RUN_TEST(test_mcp_server_call_tool_mock);
    HU_RUN_TEST(test_mcp_init_tools_mock);

    HU_TEST_SUITE("MCP Host (Server)");
    HU_RUN_TEST(test_mcp_host_create_destroy);
    HU_RUN_TEST(test_mcp_host_create_null_alloc);
    HU_RUN_TEST(test_mcp_host_create_null_out);
    HU_RUN_TEST(test_mcp_host_create_zero_tools);
    HU_RUN_TEST(test_mcp_host_run_null);
    HU_RUN_TEST(test_mcp_host_destroy_null);
    HU_RUN_TEST(test_mcp_host_create_with_memory);
    HU_RUN_TEST(test_mcp_host_create_many_tools);
    HU_RUN_TEST(test_mcp_host_create_null_alloc_rejected);
    HU_RUN_TEST(test_mcp_server_null_config_fails);
    HU_RUN_TEST(test_mcp_server_create_null_command_fails);
    HU_RUN_TEST(test_mcp_server_create_null_alloc_fails);
    HU_RUN_TEST(test_mcp_server_call_tool_nonexistent);
    HU_RUN_TEST(test_mcp_server_double_connect);
    HU_RUN_TEST(test_mcp_server_list_tools_not_connected);
    HU_RUN_TEST(test_mcp_init_tools_null_alloc);
    HU_RUN_TEST(test_mcp_init_tools_zero_configs);
    HU_RUN_TEST(test_mcp_init_tools_null_out_rejected);
    HU_RUN_TEST(test_mcp_free_tools_null_safe);
}
