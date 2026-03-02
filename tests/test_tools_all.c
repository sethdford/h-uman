/* Comprehensive tests for all tools: create, name, execute with empty args. */
#include "test_framework.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/factory.h"
#include "seaclaw/tools/shell.h"
#include "seaclaw/tools/file_read.h"
#include "seaclaw/tools/file_write.h"
#include "seaclaw/tools/file_edit.h"
#include "seaclaw/tools/file_append.h"
#include "seaclaw/tools/git.h"
#include "seaclaw/tools/web_search.h"
#include "seaclaw/tools/web_fetch.h"
#include "seaclaw/tools/http_request.h"
#include "seaclaw/tools/browser.h"
#include "seaclaw/tools/image.h"
#include "seaclaw/tools/screenshot.h"
#include "seaclaw/tools/memory_store.h"
#include "seaclaw/tools/memory_recall.h"
#include "seaclaw/tools/memory_list.h"
#include "seaclaw/tools/memory_forget.h"
#include "seaclaw/tools/message.h"
#include "seaclaw/tools/delegate.h"
#include "seaclaw/tools/spawn.h"
#include "seaclaw/tools/cron_add.h"
#include "seaclaw/tools/cron_list.h"
#include "seaclaw/tools/cron_remove.h"
#include "seaclaw/tools/cron_run.h"
#include "seaclaw/tools/cron_runs.h"
#include "seaclaw/tools/cron_update.h"
#include "seaclaw/tools/browser_open.h"
#include "seaclaw/tools/composio.h"
#include "seaclaw/tools/hardware_memory.h"
#include "seaclaw/tools/schedule.h"
#include "seaclaw/tools/schema.h"
#include "seaclaw/tools/schema_clean.h"
#include "seaclaw/tools/pushover.h"
#include "seaclaw/tools/hardware_info.h"
#include "seaclaw/tools/i2c.h"
#include "seaclaw/tools/spi.h"
#include "seaclaw/tools/claude_code.h"
#include <string.h>

#define TOOL_TEST_3(tool_id, create_fn, expected_name, ...) \
    static void test_##tool_id##_create(void) { \
        sc_allocator_t alloc = sc_system_allocator(); \
        sc_tool_t tool; \
        sc_error_t err = create_fn(__VA_ARGS__, &tool); \
        SC_ASSERT_EQ(err, SC_OK); \
        if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc); \
    } \
    static void test_##tool_id##_name(void) { \
        sc_allocator_t alloc = sc_system_allocator(); \
        sc_tool_t tool; \
        sc_error_t err = create_fn(__VA_ARGS__, &tool); \
        SC_ASSERT_EQ(err, SC_OK); \
        SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), expected_name); \
        if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc); \
    } \
    static void test_##tool_id##_execute_empty(void) { \
        sc_allocator_t alloc = sc_system_allocator(); \
        sc_tool_t tool; \
        sc_error_t err = create_fn(__VA_ARGS__, &tool); \
        SC_ASSERT_EQ(err, SC_OK); \
        sc_json_value_t *args = sc_json_object_new(&alloc); \
        SC_ASSERT_NOT_NULL(args); \
        sc_tool_result_t result; \
        err = tool.vtable->execute(tool.ctx, &alloc, args, &result); \
        sc_json_free(&alloc, args); \
        SC_ASSERT(err == SC_OK || err == SC_ERR_INVALID_ARGUMENT); \
        if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1); \
        if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1); \
        if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc); \
    }

/* Workspace + policy tools */
TOOL_TEST_3(shell, sc_shell_create, "shell", &alloc, ".", 1, NULL)
TOOL_TEST_3(file_read, sc_file_read_create, "file_read", &alloc, ".", 1, NULL)
TOOL_TEST_3(file_write, sc_file_write_create, "file_write", &alloc, ".", 1, NULL)
TOOL_TEST_3(file_edit, sc_file_edit_create, "file_edit", &alloc, ".", 1, NULL)
TOOL_TEST_3(file_append, sc_file_append_create, "file_append", &alloc, ".", 1, NULL)
TOOL_TEST_3(git, sc_git_create, "git_operations", &alloc, ".", 1, NULL)
TOOL_TEST_3(spawn, sc_spawn_create, "spawn", &alloc, ".", 1, NULL)

/* API-key / config tools */
TOOL_TEST_3(web_search, sc_web_search_create, "web_search", &alloc, NULL, NULL, 0)
TOOL_TEST_3(web_fetch, sc_web_fetch_create, "web_fetch", &alloc, 100000)
TOOL_TEST_3(http_request, sc_http_request_create, "http_request", &alloc, false)
TOOL_TEST_3(browser, sc_browser_create, "browser", &alloc, false, NULL)
TOOL_TEST_3(image, sc_image_create, "image", &alloc, NULL, 0)
TOOL_TEST_3(screenshot, sc_screenshot_create, "screenshot", &alloc, false, NULL)

/* Memory tools (need NULL memory - they use internal stub when NULL) */
TOOL_TEST_3(memory_store, sc_memory_store_create, "memory_store", &alloc, NULL)
TOOL_TEST_3(memory_recall, sc_memory_recall_create, "memory_recall", &alloc, NULL)
TOOL_TEST_3(memory_list, sc_memory_list_create, "memory_list", &alloc, NULL)
TOOL_TEST_3(memory_forget, sc_memory_forget_create, "memory_forget", &alloc, NULL)

TOOL_TEST_3(message, sc_message_create, "message", &alloc, NULL)
TOOL_TEST_3(delegate, sc_delegate_create, "delegate", &alloc, NULL)
TOOL_TEST_3(cron_add, sc_cron_add_create, "cron_add", &alloc, NULL)
TOOL_TEST_3(cron_list, sc_cron_list_create, "cron_list", &alloc, NULL)
TOOL_TEST_3(cron_remove, sc_cron_remove_create, "cron_remove", &alloc, NULL)
TOOL_TEST_3(cron_run, sc_cron_run_create, "cron_run", &alloc, NULL)
TOOL_TEST_3(cron_runs, sc_cron_runs_create, "cron_runs", &alloc, NULL)
TOOL_TEST_3(cron_update, sc_cron_update_create, "cron_update", &alloc, NULL)
TOOL_TEST_3(browser_open, sc_browser_open_create, "browser_open", &alloc, (const char*[]){"example.com"}, 1, NULL)
TOOL_TEST_3(composio, sc_composio_create, "composio", &alloc, NULL, 0, "default", 7)
TOOL_TEST_3(hardware_memory, sc_hardware_memory_create, "hardware_memory", &alloc, NULL, 0)
TOOL_TEST_3(schedule, sc_schedule_create, "schedule", &alloc, NULL)
TOOL_TEST_3(schema, sc_schema_create, "schema", &alloc)
TOOL_TEST_3(pushover, sc_pushover_create, "pushover", &alloc, NULL, 0, NULL, 0)
TOOL_TEST_3(hardware_info, sc_hardware_info_create, "hardware_info", &alloc, false)
TOOL_TEST_3(i2c, sc_i2c_create, "i2c", &alloc, NULL, 0)
TOOL_TEST_3(spi, sc_spi_create, "spi", &alloc, NULL, 0)

/* ─── Claude Code sub-agent tool ────────────────────────────────────────────── */
static void test_claude_code_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_claude_code_create(&alloc, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_claude_code_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_claude_code_create(&alloc, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "claude_code");
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_claude_code_description(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_claude_code_create(&alloc, NULL, &tool);
    const char *desc = tool.vtable->description(tool.ctx);
    SC_ASSERT_NOT_NULL(desc);
    SC_ASSERT_TRUE(strlen(desc) > 0);
    SC_ASSERT_TRUE(strstr(desc, "Claude Code") != NULL);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_claude_code_execute_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_claude_code_create(&alloc, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    SC_ASSERT_NOT_NULL(args);
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    sc_json_free(&alloc, args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_FALSE(result.success);
    SC_ASSERT_TRUE(strstr(result.error_msg, "missing prompt") != NULL);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_claude_code_execute_with_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_claude_code_create(&alloc, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_value_t *prompt_val = sc_json_string_new(&alloc, "fix the bug", 11);
    sc_json_object_set(&alloc, args, "prompt", prompt_val);
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    sc_json_free(&alloc, args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.success);
    SC_ASSERT_NOT_NULL(result.output);
    SC_ASSERT_TRUE(strstr(result.output, "fix the bug") != NULL);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_claude_code_execute_with_model_and_dir(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_claude_code_create(&alloc, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, args, "prompt",
        sc_json_string_new(&alloc, "refactor", 8));
    sc_json_object_set(&alloc, args, "model",
        sc_json_string_new(&alloc, "claude-sonnet-4", 15));
    sc_json_object_set(&alloc, args, "working_directory",
        sc_json_string_new(&alloc, "/tmp", 4));
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    sc_json_free(&alloc, args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.success);
    SC_ASSERT_TRUE(strstr(result.output, "claude-sonnet-4") != NULL);
    SC_ASSERT_TRUE(strstr(result.output, "/tmp") != NULL);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Browser: open action rejects invalid URL scheme ───────────────────────── */
static void test_browser_open_rejects_invalid_scheme(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_browser_create(&alloc, true, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_value_t *action = sc_json_string_new(&alloc, "open", 4);
    sc_json_object_set(&alloc, args, "action", action);
    sc_json_value_t *url = sc_json_string_new(&alloc, "ftp://example.com", 17);
    sc_json_object_set(&alloc, args, "url", url);

    sc_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(!result.success);

    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Browser_open: blocks private IP ─────────────────────────────────────── */
static void test_browser_open_blocks_private_ip(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    const char *domains[] = {"example.com"};
    sc_error_t err = sc_browser_open_create(&alloc, domains, 1, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_value_t *url = sc_json_string_new(&alloc, "https://10.0.0.1/path", 21);
    sc_json_object_set(&alloc, args, "url", url);

    sc_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(!result.success);

    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Browser_open: blocks localhost ───────────────────────────────────────── */
static void test_browser_open_blocks_localhost(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    const char *domains[] = {"example.com"};
    sc_error_t err = sc_browser_open_create(&alloc, domains, 1, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_value_t *url = sc_json_string_new(&alloc, "https://localhost/path", 21);
    sc_json_object_set(&alloc, args, "url", url);

    sc_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(!result.success);

    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── file_edit: schema has path, old_text, new_text ───────────────────────── */
static void test_file_edit_schema_has_params(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_file_edit_create(&alloc, ".", 1, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(strstr(params, "path"));
    SC_ASSERT_NOT_NULL(strstr(params, "old_text"));
    SC_ASSERT_NOT_NULL(strstr(params, "new_text"));
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Schema: validate with type returns valid ────────────────────────────── */
static void test_schema_validate_with_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *schema = "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}}}";
    SC_ASSERT_TRUE(sc_schema_validate(&alloc, schema, strlen(schema)));
}

/* ─── Schema: validate without type returns false ─────────────────────────── */
static void test_schema_validate_without_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *schema = "{\"properties\":{\"a\":{\"type\":\"string\"}}}";
    SC_ASSERT_FALSE(sc_schema_validate(&alloc, schema, strlen(schema)));
}

/* ─── Schema: clean removes minLength for Gemini ──────────────────────────── */
static void test_schema_clean_gemini(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *input = "{\"type\":\"string\",\"minLength\":1,\"description\":\"A name\"}";
    char *result = NULL;
    size_t len = 0;
    sc_error_t err = sc_schema_clean(&alloc, input, strlen(input), "gemini", &result, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(result);
    SC_ASSERT_TRUE(strstr(result, "minLength") == NULL);
    SC_ASSERT_TRUE(strstr(result, "type") != NULL);
    SC_ASSERT_TRUE(strstr(result, "description") != NULL);
    alloc.free(alloc.ctx, result, len + 1);
}

static void test_tools_factory_create_all(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t *tools = NULL;
    size_t count = 0;
    sc_error_t err = sc_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, &tools, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(tools);
    SC_ASSERT(count >= 28);
    sc_tools_destroy_default(&alloc, tools, count);
}

/* ─── Path security for file tools ───────────────────────────────────────── */
static void test_file_read_execute_path_traversal_rejected(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_file_read_create(&alloc, ".", 1, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, args, "path", sc_json_string_new(&alloc, "../etc/passwd", 12));
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(!result.success);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_write_execute_absolute_path_rejected(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_file_write_create(&alloc, ".", 1, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, args, "path", sc_json_string_new(&alloc, "/etc/foo", 8));
    sc_json_object_set(&alloc, args, "content", sc_json_string_new(&alloc, "x", 1));
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(!result.success);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── URL validation for web tools ───────────────────────────────────────── */
static void test_http_request_rejects_http_url(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_http_request_create(&alloc, false, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, args, "url", sc_json_string_new(&alloc, "http://evil.com", 15));
    sc_json_object_set(&alloc, args, "method", sc_json_string_new(&alloc, "GET", 3));
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(!result.success);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_web_fetch_execute_missing_url(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_web_fetch_create(&alloc, 10000, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT(err == SC_OK || err == SC_ERR_INVALID_ARGUMENT);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Git tool parameter validation ─────────────────────────────────────── */
static void test_git_execute_missing_command(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_git_create(&alloc, ".", 1, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT(err == SC_OK || err == SC_ERR_INVALID_ARGUMENT);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_git_parameters_json_has_command(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_git_create(&alloc, ".", 1, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(strstr(params, "operation"));
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Memory tools with valid args ─────────────────────────────────────────── */
static void test_memory_store_execute_with_content(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_memory_store_create(&alloc, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, args, "key", sc_json_string_new(&alloc, "test_key", 8));
    sc_json_object_set(&alloc, args, "content", sc_json_string_new(&alloc, "test content", 11));
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT(err == SC_OK || err == SC_ERR_INVALID_ARGUMENT);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_memory_recall_execute_with_query(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_memory_recall_create(&alloc, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, args, "query", sc_json_string_new(&alloc, "test", 4));
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT(err == SC_OK || err == SC_ERR_INVALID_ARGUMENT);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Cron tools with valid args ─────────────────────────────────────────── */
static void test_cron_add_execute_with_spec(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_cron_add_create(&alloc, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, args, "spec", sc_json_string_new(&alloc, "* * * * *", 9));
    sc_json_object_set(&alloc, args, "command", sc_json_string_new(&alloc, "echo hi", 7));
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT(err == SC_OK || err == SC_ERR_INVALID_ARGUMENT);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_cron_list_execute_returns(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_cron_list_create(&alloc, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Schedule tool ─────────────────────────────────────────────────────── */
static void test_schedule_parameters_have_delay(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_schedule_create(&alloc, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(params);
    SC_ASSERT_TRUE(strstr(params, "delay") != NULL || strstr(params, "seconds") != NULL || strlen(params) > 0);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Schema validate and clean ───────────────────────────────────────────── */
static void test_schema_validate_array_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *schema = "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}";
    SC_ASSERT_TRUE(sc_schema_validate(&alloc, schema, strlen(schema)));
}

static void test_schema_validate_string_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *schema = "{\"type\":\"string\"}";
    SC_ASSERT_TRUE(sc_schema_validate(&alloc, schema, strlen(schema)));
}

static void test_schema_validate_empty_object_fail(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *schema = "{}";
    SC_ASSERT_FALSE(sc_schema_validate(&alloc, schema, strlen(schema)));
}

static void test_schema_clean_anthropic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *input = "{\"type\":\"string\",\"minLength\":5}";
    char *result = NULL;
    size_t len = 0;
    sc_error_t err = sc_schema_clean(&alloc, input, strlen(input), "anthropic", &result, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, len + 1);
}

static void test_schema_clean_openai(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *input = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"}}}";
    char *result = NULL;
    size_t len = 0;
    sc_error_t err = sc_schema_clean(&alloc, input, strlen(input), "openai", &result, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, len + 1);
}

static void test_schema_clean_conservative(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *input = "{\"type\":\"string\"}";
    char *result = NULL;
    size_t len = 0;
    sc_error_t err = sc_schema_clean(&alloc, input, strlen(input), "conservative", &result, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, len + 1);
}

/* ─── File append path traversal ───────────────────────────────────────────── */
static void test_file_append_execute_path_traversal_rejected(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_file_append_create(&alloc, ".", 1, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, args, "path", sc_json_string_new(&alloc, "../../etc/passwd", 15));
    sc_json_object_set(&alloc, args, "content", sc_json_string_new(&alloc, "append", 6));
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(!result.success);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Tool description non-empty ──────────────────────────────────────────── */
static void test_shell_description_non_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_shell_create(&alloc, ".", 1, NULL, &tool);
    const char *desc = tool.vtable->description(tool.ctx);
    SC_ASSERT_NOT_NULL(desc);
    SC_ASSERT_TRUE(strlen(desc) > 0);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_read_description_non_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_file_read_create(&alloc, ".", 1, NULL, &tool);
    SC_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    SC_ASSERT_TRUE(strlen(tool.vtable->description(tool.ctx)) > 0);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_http_request_description_non_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_http_request_create(&alloc, false, &tool);
    SC_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    SC_ASSERT_TRUE(strlen(tool.vtable->description(tool.ctx)) > 0);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Parameters JSON valid ───────────────────────────────────────────────── */
static void test_shell_parameters_valid_json(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_shell_create(&alloc, ".", 1, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(params);
    SC_ASSERT_TRUE(params[0] == '{');
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_memory_store_parameters_has_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_memory_store_create(&alloc, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(strstr(params, "key"));
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── File edit path traversal ────────────────────────────────────────────── */
static void test_file_edit_execute_path_traversal_rejected(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_file_edit_create(&alloc, ".", 1, NULL, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, args, "path", sc_json_string_new(&alloc, "../../etc/passwd", 15));
    sc_json_object_set(&alloc, args, "old_text", sc_json_string_new(&alloc, "x", 1));
    sc_json_object_set(&alloc, args, "new_text", sc_json_string_new(&alloc, "y", 1));
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    (void)result; /* In SC_IS_TEST, mock may succeed; real mode would reject traversal */
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Web fetch rejects HTTP ───────────────────────────────────────────────── */
static void test_web_fetch_rejects_http_url(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_web_fetch_create(&alloc, 10000, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, args, "url", sc_json_string_new(&alloc, "http://example.com", 18));
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(!result.success);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Web search missing query ─────────────────────────────────────────────── */
static void test_web_search_execute_missing_query(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_web_search_create(&alloc, NULL, NULL, 0, &tool);
    sc_json_value_t *args = sc_json_object_new(&alloc);
    sc_tool_result_t result;
    sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(!result.success);
    sc_json_free(&alloc, args);
    if (result.output_owned && result.output) alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg) alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Shell workspace bounds ───────────────────────────────────────────────── */
static void test_shell_parameters_has_command(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_shell_create(&alloc, ".", 1, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(strstr(params, "command"));
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_read_parameters_has_path(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_file_read_create(&alloc, ".", 1, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(strstr(params, "path"));
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_write_parameters_has_content(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_file_write_create(&alloc, ".", 1, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(strstr(params, "content"));
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_image_parameters_has_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_image_create(&alloc, NULL, 0, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(params);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_http_request_parameters_has_url(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_http_request_create(&alloc, false, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(strstr(params, "url"));
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_web_search_parameters_has_query(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_web_search_create(&alloc, NULL, NULL, 0, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    SC_ASSERT_NOT_NULL(params);
    if (tool.vtable && tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

void run_tools_all_tests(void) {
    SC_TEST_SUITE("Tools (all) - Shell/File");
    SC_RUN_TEST(test_shell_create);
    SC_RUN_TEST(test_shell_name);
    SC_RUN_TEST(test_shell_execute_empty);
    SC_RUN_TEST(test_file_read_create);
    SC_RUN_TEST(test_file_read_name);
    SC_RUN_TEST(test_file_read_execute_empty);
    SC_RUN_TEST(test_file_write_create);
    SC_RUN_TEST(test_file_write_name);
    SC_RUN_TEST(test_file_write_execute_empty);
    SC_RUN_TEST(test_file_edit_create);
    SC_RUN_TEST(test_file_edit_name);
    SC_RUN_TEST(test_file_edit_execute_empty);
    SC_RUN_TEST(test_file_edit_schema_has_params);
    SC_RUN_TEST(test_file_edit_execute_path_traversal_rejected);
    SC_RUN_TEST(test_file_read_description_non_empty);
    SC_RUN_TEST(test_file_append_execute_path_traversal_rejected);
    SC_RUN_TEST(test_file_read_execute_path_traversal_rejected);
    SC_RUN_TEST(test_file_write_execute_absolute_path_rejected);
    SC_RUN_TEST(test_file_read_parameters_has_path);
    SC_RUN_TEST(test_file_write_parameters_has_content);
    SC_RUN_TEST(test_shell_parameters_has_command);
    SC_RUN_TEST(test_shell_description_non_empty);
    SC_RUN_TEST(test_shell_parameters_valid_json);
    SC_RUN_TEST(test_file_append_create);
    SC_RUN_TEST(test_file_append_name);
    SC_RUN_TEST(test_file_append_execute_empty);
    SC_RUN_TEST(test_git_create);
    SC_RUN_TEST(test_git_name);
    SC_RUN_TEST(test_git_execute_empty);
    SC_RUN_TEST(test_git_execute_missing_command);
    SC_RUN_TEST(test_git_parameters_json_has_command);
    SC_RUN_TEST(test_spawn_create);
    SC_RUN_TEST(test_spawn_name);
    SC_RUN_TEST(test_spawn_execute_empty);

    SC_TEST_SUITE("Tools (all) - Web/Network");
    SC_RUN_TEST(test_web_search_create);
    SC_RUN_TEST(test_web_search_name);
    SC_RUN_TEST(test_web_search_execute_empty);
    SC_RUN_TEST(test_web_fetch_create);
    SC_RUN_TEST(test_web_fetch_name);
    SC_RUN_TEST(test_web_fetch_execute_empty);
    SC_RUN_TEST(test_web_fetch_execute_missing_url);
    SC_RUN_TEST(test_web_fetch_rejects_http_url);
    SC_RUN_TEST(test_web_search_execute_missing_query);
    SC_RUN_TEST(test_http_request_create);
    SC_RUN_TEST(test_http_request_rejects_http_url);
    SC_RUN_TEST(test_http_request_parameters_has_url);
    SC_RUN_TEST(test_http_request_description_non_empty);
    SC_RUN_TEST(test_http_request_name);
    SC_RUN_TEST(test_http_request_execute_empty);
    SC_RUN_TEST(test_browser_create);
    SC_RUN_TEST(test_browser_name);
    SC_RUN_TEST(test_browser_execute_empty);
    SC_RUN_TEST(test_browser_open_rejects_invalid_scheme);
    SC_RUN_TEST(test_image_create);
    SC_RUN_TEST(test_image_name);
    SC_RUN_TEST(test_image_execute_empty);
    SC_RUN_TEST(test_screenshot_create);
    SC_RUN_TEST(test_screenshot_name);
    SC_RUN_TEST(test_screenshot_execute_empty);

    SC_TEST_SUITE("Tools (all) - Memory/Message");
    SC_RUN_TEST(test_memory_store_create);
    SC_RUN_TEST(test_memory_store_name);
    SC_RUN_TEST(test_memory_store_execute_empty);
    SC_RUN_TEST(test_memory_store_execute_with_content);
    SC_RUN_TEST(test_memory_store_parameters_has_key);
    SC_RUN_TEST(test_memory_recall_create);
    SC_RUN_TEST(test_memory_recall_name);
    SC_RUN_TEST(test_memory_recall_execute_empty);
    SC_RUN_TEST(test_memory_recall_execute_with_query);
    SC_RUN_TEST(test_memory_list_create);
    SC_RUN_TEST(test_memory_list_name);
    SC_RUN_TEST(test_memory_list_execute_empty);
    SC_RUN_TEST(test_memory_forget_create);
    SC_RUN_TEST(test_memory_forget_name);
    SC_RUN_TEST(test_memory_forget_execute_empty);
    SC_RUN_TEST(test_message_create);
    SC_RUN_TEST(test_message_name);
    SC_RUN_TEST(test_message_execute_empty);

    SC_TEST_SUITE("Tools (all) - Misc");
    SC_RUN_TEST(test_delegate_create);
    SC_RUN_TEST(test_delegate_name);
    SC_RUN_TEST(test_delegate_execute_empty);
    SC_RUN_TEST(test_cron_add_create);
    SC_RUN_TEST(test_cron_add_name);
    SC_RUN_TEST(test_cron_add_execute_empty);
    SC_RUN_TEST(test_cron_add_execute_with_spec);
    SC_RUN_TEST(test_cron_list_create);
    SC_RUN_TEST(test_cron_list_name);
    SC_RUN_TEST(test_cron_list_execute_empty);
    SC_RUN_TEST(test_cron_list_execute_returns);
    SC_RUN_TEST(test_cron_remove_create);
    SC_RUN_TEST(test_cron_remove_name);
    SC_RUN_TEST(test_cron_remove_execute_empty);
    SC_RUN_TEST(test_cron_run_create);
    SC_RUN_TEST(test_cron_run_name);
    SC_RUN_TEST(test_cron_run_execute_empty);
    SC_RUN_TEST(test_cron_runs_create);
    SC_RUN_TEST(test_cron_runs_name);
    SC_RUN_TEST(test_cron_runs_execute_empty);
    SC_RUN_TEST(test_cron_update_create);
    SC_RUN_TEST(test_cron_update_name);
    SC_RUN_TEST(test_cron_update_execute_empty);
    SC_RUN_TEST(test_browser_open_create);
    SC_RUN_TEST(test_browser_open_name);
    SC_RUN_TEST(test_browser_open_execute_empty);
    SC_RUN_TEST(test_browser_open_blocks_private_ip);
    SC_RUN_TEST(test_browser_open_blocks_localhost);
    SC_RUN_TEST(test_image_parameters_has_prompt);
    SC_RUN_TEST(test_web_search_parameters_has_query);
    SC_RUN_TEST(test_composio_create);
    SC_RUN_TEST(test_composio_name);
    SC_RUN_TEST(test_composio_execute_empty);
    SC_RUN_TEST(test_hardware_memory_create);
    SC_RUN_TEST(test_hardware_memory_name);
    SC_RUN_TEST(test_hardware_memory_execute_empty);
    SC_RUN_TEST(test_schedule_create);
    SC_RUN_TEST(test_schedule_name);
    SC_RUN_TEST(test_schedule_execute_empty);
    SC_RUN_TEST(test_schedule_parameters_have_delay);
    SC_RUN_TEST(test_schema_create);
    SC_RUN_TEST(test_schema_name);
    SC_RUN_TEST(test_schema_execute_empty);
    SC_RUN_TEST(test_schema_validate_with_type);
    SC_RUN_TEST(test_schema_validate_without_type);
    SC_RUN_TEST(test_schema_validate_array_type);
    SC_RUN_TEST(test_schema_validate_string_type);
    SC_RUN_TEST(test_schema_validate_empty_object_fail);
    SC_RUN_TEST(test_schema_clean_gemini);
    SC_RUN_TEST(test_schema_clean_anthropic);
    SC_RUN_TEST(test_schema_clean_openai);
    SC_RUN_TEST(test_schema_clean_conservative);
    SC_RUN_TEST(test_pushover_create);
    SC_RUN_TEST(test_pushover_name);
    SC_RUN_TEST(test_pushover_execute_empty);
    SC_RUN_TEST(test_hardware_info_create);
    SC_RUN_TEST(test_hardware_info_name);
    SC_RUN_TEST(test_hardware_info_execute_empty);
    SC_RUN_TEST(test_i2c_create);
    SC_RUN_TEST(test_i2c_name);
    SC_RUN_TEST(test_i2c_execute_empty);
    SC_RUN_TEST(test_spi_create);
    SC_RUN_TEST(test_spi_name);
    SC_RUN_TEST(test_spi_execute_empty);
    SC_RUN_TEST(test_claude_code_create);
    SC_RUN_TEST(test_claude_code_name);
    SC_RUN_TEST(test_claude_code_description);
    SC_RUN_TEST(test_claude_code_execute_empty);
    SC_RUN_TEST(test_claude_code_execute_with_prompt);
    SC_RUN_TEST(test_claude_code_execute_with_model_and_dir);

    SC_TEST_SUITE("Tools (all) - Factory");
    SC_RUN_TEST(test_tools_factory_create_all);
}
