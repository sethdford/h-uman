#include "test_framework.h"
#include "seaclaw/mcp.h"
#include "seaclaw/mcp_server.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/tool.h"
#include <string.h>

static void test_mcp_server_create_destroy(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_mcp_server_config_t cfg = { .command = "echo", .args = NULL, .args_count = 0 };
    sc_mcp_server_t *srv = sc_mcp_server_create(&alloc, &cfg);
    SC_ASSERT_NOT_NULL(srv);
    sc_mcp_server_destroy(srv);
}

static void test_mcp_server_connect_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_mcp_server_config_t cfg = { .command = "echo", .args = NULL, .args_count = 0 };
    sc_mcp_server_t *srv = sc_mcp_server_create(&alloc, &cfg);
    SC_ASSERT_NOT_NULL(srv);
    sc_error_t err = sc_mcp_server_connect(srv);
    SC_ASSERT_EQ(err, SC_OK);
    sc_mcp_server_destroy(srv);
}

static void test_mcp_server_list_tools_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_mcp_server_config_t cfg = { .command = "echo", .args = NULL, .args_count = 0 };
    sc_mcp_server_t *srv = sc_mcp_server_create(&alloc, &cfg);
    sc_mcp_server_connect(srv);
    char **names = NULL, **descs = NULL;
    size_t count = 0;
    sc_error_t err = sc_mcp_server_list_tools(srv, &alloc, &names, &descs, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 1u);
    SC_ASSERT_STR_EQ(names[0], "mock_tool");
    for (size_t i = 0; i < count; i++) {
        alloc.free(alloc.ctx, names[i], strlen(names[i]) + 1);
        alloc.free(alloc.ctx, descs[i], strlen(descs[i]) + 1);
    }
    alloc.free(alloc.ctx, names, count * sizeof(char *));
    alloc.free(alloc.ctx, descs, count * sizeof(char *));
    sc_mcp_server_destroy(srv);
}

static void test_mcp_server_call_tool_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_mcp_server_config_t cfg = { .command = "echo", .args = NULL, .args_count = 0 };
    sc_mcp_server_t *srv = sc_mcp_server_create(&alloc, &cfg);
    sc_mcp_server_connect(srv);
    char *result = NULL;
    size_t result_len = 0;
    sc_error_t err = sc_mcp_server_call_tool(srv, &alloc, "mock_tool", "{}",
        &result, &result_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, result_len + 1);
    sc_mcp_server_destroy(srv);
}

static void test_mcp_init_tools_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_mcp_server_config_t cfg = { .command = "echo", .args = NULL, .args_count = 0 };
    sc_tool_t *tools = NULL;
    size_t count = 0;
    sc_error_t err = sc_mcp_init_tools(&alloc, &cfg, 1, &tools, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count >= 1);
    sc_mcp_free_tools(&alloc, tools, count);
}

/* ── MCP Host (server mode) tests ────────────────────────────────────────── */

static sc_error_t mock_tool_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    (void)ctx; (void)args;
    const char *msg = "mock ok";
    size_t len = strlen(msg);
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    memcpy(buf, msg, len + 1);
    *out = sc_tool_result_ok_owned(buf, len);
    return SC_OK;
}
static const char *mock_tool_name(void *ctx) { (void)ctx; return "test_tool"; }
static const char *mock_tool_desc(void *ctx) { (void)ctx; return "A test tool"; }
static const char *mock_tool_params(void *ctx) { (void)ctx; return "{\"type\":\"object\",\"properties\":{}}"; }

static const sc_tool_vtable_t mock_host_vtable = {
    .execute = mock_tool_execute,
    .name = mock_tool_name,
    .description = mock_tool_desc,
    .parameters_json = mock_tool_params,
    .deinit = NULL,
};

static void test_mcp_host_create_destroy(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tools[1];
    tools[0].ctx = NULL;
    tools[0].vtable = &mock_host_vtable;

    sc_mcp_host_t *host = NULL;
    sc_error_t err = sc_mcp_host_create(&alloc, tools, 1, &host);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(host);
    sc_mcp_host_destroy(host);
}

static void test_mcp_host_create_null_alloc(void) {
    sc_mcp_host_t *host = NULL;
    sc_error_t err = sc_mcp_host_create(NULL, NULL, 0, &host);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_mcp_host_create_null_out(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_mcp_host_create(&alloc, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_mcp_host_create_zero_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_mcp_host_t *host = NULL;
    sc_error_t err = sc_mcp_host_create(&alloc, NULL, 0, &host);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(host);
    sc_mcp_host_destroy(host);
}

static void test_mcp_host_run_null(void) {
    sc_error_t err = sc_mcp_host_run(NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_mcp_host_destroy_null(void) {
    sc_mcp_host_destroy(NULL); /* should not crash */
}

void run_mcp_tests(void) {
    SC_TEST_SUITE("MCP Client");
    SC_RUN_TEST(test_mcp_server_create_destroy);
    SC_RUN_TEST(test_mcp_server_connect_test_mode);
    SC_RUN_TEST(test_mcp_server_list_tools_mock);
    SC_RUN_TEST(test_mcp_server_call_tool_mock);
    SC_RUN_TEST(test_mcp_init_tools_mock);

    SC_TEST_SUITE("MCP Host (Server)");
    SC_RUN_TEST(test_mcp_host_create_destroy);
    SC_RUN_TEST(test_mcp_host_create_null_alloc);
    SC_RUN_TEST(test_mcp_host_create_null_out);
    SC_RUN_TEST(test_mcp_host_create_zero_tools);
    SC_RUN_TEST(test_mcp_host_run_null);
    SC_RUN_TEST(test_mcp_host_destroy_null);
}
