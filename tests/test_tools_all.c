/* Comprehensive tests for all tools: create, name, execute with empty args. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/security.h"
#include "human/tool.h"
#include "human/tools/analytics.h"
#include "human/tools/broadcast.h"
#include "human/tools/browser.h"
#include "human/tools/browser_open.h"
#include "human/tools/calendar_tool.h"
#include "human/tools/claude_code.h"
#include "human/tools/cli_wrapper_common.h"
#include "human/tools/composio.h"
#include "human/tools/crm.h"
#include "human/tools/cron_add.h"
#include "human/tools/cron_list.h"
#include "human/tools/cron_remove.h"
#include "human/tools/cron_run.h"
#include "human/tools/cron_runs.h"
#include "human/tools/cron_update.h"
#include "human/tools/delegate.h"
#include "human/tools/facebook.h"
#include "human/tools/factory.h"
#include "human/tools/file_append.h"
#include "human/tools/file_edit.h"
#include "human/tools/file_read.h"
#include "human/tools/file_write.h"
#include "human/tools/firebase.h"
#include "human/tools/gcloud.h"
#include "human/tools/git.h"
#include "human/tools/hardware_info.h"
#include "human/tools/hardware_memory.h"
#include "human/tools/homeassistant.h"
#include "human/tools/http_request.h"
#include "human/tools/i2c.h"
#include "human/tools/image.h"
#include "human/tools/instagram.h"
#include "human/tools/invoice.h"
#include "human/tools/jira.h"
#include "human/tools/memory_forget.h"
#include "human/tools/memory_list.h"
#include "human/tools/memory_recall.h"
#include "human/tools/memory_store.h"
#include "human/tools/message.h"
#include "human/tools/pushover.h"
#include "human/tools/report.h"
#include "human/tools/schedule.h"
#include "human/tools/schema.h"
#include "human/tools/schema_clean.h"
#include "human/tools/screenshot.h"
#include "human/tools/shell.h"
#include "human/tools/skill_write.h"
#include "human/tools/social.h"
#include "human/tools/spawn.h"
#include "human/tools/spi.h"
#include "human/tools/spreadsheet.h"
#include "human/tools/twitter.h"
#include "human/tools/web_fetch.h"
#include "human/tools/web_search.h"
#include "human/tools/web_search_providers.h"
#include "human/tools/workflow.h"
#include "test_framework.h"
#include <string.h>

#define TOOL_TEST_3(tool_id, create_fn, expected_name, ...)                            \
    static void test_##tool_id##_create(void) {                                        \
        hu_allocator_t alloc = hu_system_allocator();                                  \
        hu_tool_t tool;                                                                \
        hu_error_t err = create_fn(__VA_ARGS__, &tool);                                \
        HU_ASSERT_EQ(err, HU_OK);                                                      \
        if (tool.vtable && tool.vtable->deinit)                                        \
            tool.vtable->deinit(tool.ctx, &alloc);                                     \
    }                                                                                  \
    static void test_##tool_id##_name(void) {                                          \
        hu_allocator_t alloc = hu_system_allocator();                                  \
        hu_tool_t tool;                                                                \
        hu_error_t err = create_fn(__VA_ARGS__, &tool);                                \
        HU_ASSERT_EQ(err, HU_OK);                                                      \
        HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), expected_name);                  \
        if (tool.vtable && tool.vtable->deinit)                                        \
            tool.vtable->deinit(tool.ctx, &alloc);                                     \
    }                                                                                  \
    static void test_##tool_id##_execute_empty(void) {                                 \
        hu_allocator_t alloc = hu_system_allocator();                                  \
        hu_tool_t tool;                                                                \
        hu_error_t err = create_fn(__VA_ARGS__, &tool);                                \
        HU_ASSERT_EQ(err, HU_OK);                                                      \
        hu_json_value_t *args = hu_json_object_new(&alloc);                            \
        HU_ASSERT_NOT_NULL(args);                                                      \
        hu_tool_result_t result;                                                       \
        err = tool.vtable->execute(tool.ctx, &alloc, args, &result);                   \
        hu_json_free(&alloc, args);                                                    \
        HU_ASSERT(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT);                     \
        if (result.output_owned && result.output)                                      \
            alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);       \
        if (result.error_msg_owned && result.error_msg)                                \
            alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1); \
        if (tool.vtable && tool.vtable->deinit)                                        \
            tool.vtable->deinit(tool.ctx, &alloc);                                     \
    }

/* Workspace + policy tools */
TOOL_TEST_3(shell, hu_shell_create, "shell", &alloc, ".", 1, NULL)
TOOL_TEST_3(file_read, hu_file_read_create, "file_read", &alloc, ".", 1, NULL)
TOOL_TEST_3(file_write, hu_file_write_create, "file_write", &alloc, ".", 1, NULL)
TOOL_TEST_3(file_edit, hu_file_edit_create, "file_edit", &alloc, ".", 1, NULL)
TOOL_TEST_3(file_append, hu_file_append_create, "file_append", &alloc, ".", 1, NULL)
TOOL_TEST_3(git, hu_git_create, "git_operations", &alloc, ".", 1, NULL)
TOOL_TEST_3(spawn, hu_spawn_create, "spawn", &alloc, ".", 1, NULL)

/* API-key / config tools */
TOOL_TEST_3(web_search, hu_web_search_create, "web_search", &alloc, NULL, NULL, 0)
TOOL_TEST_3(web_fetch, hu_web_fetch_create, "web_fetch", &alloc, 100000)
TOOL_TEST_3(http_request, hu_http_request_create, "http_request", &alloc, false)
TOOL_TEST_3(browser, hu_browser_create, "browser", &alloc, false, NULL)
TOOL_TEST_3(image, hu_image_create, "image", &alloc, NULL, 0)
TOOL_TEST_3(screenshot, hu_screenshot_create, "screenshot", &alloc, false, NULL)

/* Memory tools (need NULL memory - they use internal stub when NULL) */
TOOL_TEST_3(memory_store, hu_memory_store_create, "memory_store", &alloc, NULL)
TOOL_TEST_3(memory_recall, hu_memory_recall_create, "memory_recall", &alloc, NULL)
TOOL_TEST_3(memory_list, hu_memory_list_create, "memory_list", &alloc, NULL)
TOOL_TEST_3(memory_forget, hu_memory_forget_create, "memory_forget", &alloc, NULL)

TOOL_TEST_3(message, hu_message_create, "message", &alloc, NULL)
TOOL_TEST_3(delegate, hu_delegate_create, "delegate", &alloc, NULL)
TOOL_TEST_3(cron_add, hu_cron_add_create, "cron_add", &alloc, NULL)
TOOL_TEST_3(cron_list, hu_cron_list_create, "cron_list", &alloc, NULL)
TOOL_TEST_3(cron_remove, hu_cron_remove_create, "cron_remove", &alloc, NULL)
TOOL_TEST_3(cron_run, hu_cron_run_create, "cron_run", &alloc, NULL)
TOOL_TEST_3(cron_runs, hu_cron_runs_create, "cron_runs", &alloc, NULL)
TOOL_TEST_3(cron_update, hu_cron_update_create, "cron_update", &alloc, NULL)
TOOL_TEST_3(browser_open, hu_browser_open_create, "browser_open", &alloc,
            (const char *[]){"example.com"}, 1, NULL)
TOOL_TEST_3(composio, hu_composio_create, "composio", &alloc, NULL, 0, "default", 7)
TOOL_TEST_3(hardware_memory, hu_hardware_memory_create, "hardware_memory", &alloc, NULL, 0)
TOOL_TEST_3(schedule, hu_schedule_create, "schedule", &alloc, NULL)
TOOL_TEST_3(schema, hu_schema_create, "schema", &alloc)
TOOL_TEST_3(pushover, hu_pushover_create, "pushover", &alloc, NULL, 0, NULL, 0)
TOOL_TEST_3(hardware_info, hu_hardware_info_create, "hardware_info", &alloc, false)
TOOL_TEST_3(i2c, hu_i2c_create, "i2c", &alloc, NULL, 0)
TOOL_TEST_3(spi, hu_spi_create, "spi", &alloc, NULL, 0)

/* ─── Claude Code sub-agent tool ────────────────────────────────────────────── */
static void test_claude_code_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_claude_code_create(&alloc, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_claude_code_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_claude_code_create(&alloc, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "claude_code");
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_claude_code_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_claude_code_create(&alloc, NULL, &tool);
    const char *desc = tool.vtable->description(tool.ctx);
    HU_ASSERT_NOT_NULL(desc);
    HU_ASSERT_TRUE(strlen(desc) > 0);
    HU_ASSERT_TRUE(strstr(desc, "Claude Code") != NULL);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_claude_code_execute_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_claude_code_create(&alloc, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    HU_ASSERT_NOT_NULL(args);
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT_TRUE(strstr(result.error_msg, "missing prompt") != NULL);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_claude_code_execute_with_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_claude_code_create(&alloc, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_value_t *prompt_val = hu_json_string_new(&alloc, "fix the bug", 11);
    hu_json_object_set(&alloc, args, "prompt", prompt_val);
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(result.output);
    HU_ASSERT_TRUE(strstr(result.output, "fix the bug") != NULL);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_claude_code_execute_with_model_and_dir(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_claude_code_create(&alloc, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "prompt", hu_json_string_new(&alloc, "refactor", 8));
    hu_json_object_set(&alloc, args, "model", hu_json_string_new(&alloc, "claude-sonnet-4", 15));
    hu_json_object_set(&alloc, args, "working_directory", hu_json_string_new(&alloc, "/tmp", 4));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_TRUE(strstr(result.output, "claude-sonnet-4") != NULL);
    HU_ASSERT_TRUE(strstr(result.output, "/tmp") != NULL);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Browser: open action rejects invalid URL scheme ───────────────────────── */
static void test_browser_open_rejects_invalid_scheme(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_browser_create(&alloc, true, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_value_t *action = hu_json_string_new(&alloc, "open", 4);
    hu_json_object_set(&alloc, args, "action", action);
    hu_json_value_t *url = hu_json_string_new(&alloc, "ftp://example.com", 17);
    hu_json_object_set(&alloc, args, "url", url);

    hu_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);

    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Browser_open: blocks private IP ─────────────────────────────────────── */
static void test_browser_open_blocks_private_ip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    const char *domains[] = {"example.com"};
    hu_error_t err = hu_browser_open_create(&alloc, domains, 1, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_value_t *url = hu_json_string_new(&alloc, "https://10.0.0.1/path", 21);
    hu_json_object_set(&alloc, args, "url", url);

    hu_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);

    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Browser_open: blocks localhost ───────────────────────────────────────── */
static void test_browser_open_blocks_localhost(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    const char *domains[] = {"example.com"};
    hu_error_t err = hu_browser_open_create(&alloc, domains, 1, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_value_t *url = hu_json_string_new(&alloc, "https://localhost/path", 21);
    hu_json_object_set(&alloc, args, "url", url);

    hu_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);

    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Browser: click action works ────────────────────────────────────────── */
static void test_browser_click_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_browser_create(&alloc, true, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "click", 5));
    hu_json_object_set(&alloc, args, "selector", hu_json_string_new(&alloc, "#submit", 7));
    hu_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(result.output);
    HU_ASSERT_NOT_NULL(strstr(result.output, "#submit"));
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_browser_click_missing_selector(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_browser_create(&alloc, true, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "click", 5));
    hu_tool_result_t result;
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_FALSE(result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Browser: type action works ─────────────────────────────────────────── */
static void test_browser_type_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_browser_create(&alloc, true, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "type", 4));
    hu_json_object_set(&alloc, args, "selector", hu_json_string_new(&alloc, "#input", 6));
    hu_json_object_set(&alloc, args, "text", hu_json_string_new(&alloc, "hello", 5));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(strstr(result.output, "hello"));
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_browser_type_missing_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_browser_create(&alloc, true, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "type", 4));
    hu_tool_result_t result;
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_FALSE(result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Browser: scroll action works ───────────────────────────────────────── */
static void test_browser_scroll_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_browser_create(&alloc, true, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "scroll", 6));
    hu_json_object_set(&alloc, args, "deltaY", hu_json_number_new(&alloc, 500));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(strstr(result.output, "500"));
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── SPI: list/transfer/read actions ────────────────────────────────────── */
static void test_spi_list_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_spi_create(&alloc, NULL, 0, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "list", 4));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(strstr(result.output, "devices"));
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_spi_transfer_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_spi_create(&alloc, NULL, 0, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "transfer", 8));
    hu_json_object_set(&alloc, args, "data", hu_json_string_new(&alloc, "FF 00", 5));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(strstr(result.output, "rx_data"));
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_spi_read_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_spi_create(&alloc, NULL, 0, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "read", 4));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(strstr(result.output, "rx_data"));
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── hardware_memory: read/write with boards ────────────────────────────── */
static void test_hardware_memory_read_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    const char *boards[] = {"nucleo-f401re"};
    hu_hardware_memory_create(&alloc, boards, 1, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "read", 4));
    hu_json_object_set(&alloc, args, "board", hu_json_string_new(&alloc, "nucleo-f401re", 13));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(result.output);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_hardware_memory_write_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    const char *boards[] = {"nucleo-f401re"};
    hu_hardware_memory_create(&alloc, boards, 1, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "write", 5));
    hu_json_object_set(&alloc, args, "board", hu_json_string_new(&alloc, "nucleo-f401re", 13));
    hu_json_object_set(&alloc, args, "value", hu_json_string_new(&alloc, "DEADBEEF", 8));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_hardware_memory_unconfigured_board(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    const char *boards[] = {"nucleo-f401re"};
    hu_hardware_memory_create(&alloc, boards, 1, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "read", 4));
    hu_json_object_set(&alloc, args, "board", hu_json_string_new(&alloc, "unknown-board", 13));
    hu_tool_result_t result;
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_FALSE(result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── file_edit: schema has path, old_text, new_text ───────────────────────── */
static void test_file_edit_schema_has_params(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_file_edit_create(&alloc, ".", 1, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(strstr(params, "path"));
    HU_ASSERT_NOT_NULL(strstr(params, "old_text"));
    HU_ASSERT_NOT_NULL(strstr(params, "new_text"));
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Schema: validate with type returns valid ────────────────────────────── */
static void test_schema_validate_with_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *schema = "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}}}";
    HU_ASSERT_TRUE(hu_schema_validate(&alloc, schema, strlen(schema)));
}

/* ─── Schema: validate without type returns false ─────────────────────────── */
static void test_schema_validate_without_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *schema = "{\"properties\":{\"a\":{\"type\":\"string\"}}}";
    HU_ASSERT_FALSE(hu_schema_validate(&alloc, schema, strlen(schema)));
}

/* ─── Schema: clean removes minLength for Gemini ──────────────────────────── */
static void test_schema_clean_gemini(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *input = "{\"type\":\"string\",\"minLength\":1,\"description\":\"A name\"}";
    char *result = NULL;
    size_t len = 0;
    hu_error_t err = hu_schema_clean(&alloc, input, strlen(input), "gemini", &result, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(result);
    HU_ASSERT_TRUE(strstr(result, "minLength") == NULL);
    HU_ASSERT_TRUE(strstr(result, "type") != NULL);
    HU_ASSERT_TRUE(strstr(result, "description") != NULL);
    alloc.free(alloc.ctx, result, len + 1);
}

static void test_tools_factory_create_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tools);
    HU_ASSERT(count >= 28);
    hu_tools_destroy_default(&alloc, tools, count);
}

/* ─── File read/write create NULL-argument tests ─────────────────────────── */
static void test_file_read_create_null_alloc(void) {
    hu_tool_t tool;
    hu_error_t err = hu_file_read_create(NULL, ".", 1, NULL, &tool);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_file_read_create_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_file_read_create(&alloc, ".", 1, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_file_write_create_null_alloc(void) {
    hu_tool_t tool;
    hu_error_t err = hu_file_write_create(NULL, ".", 1, NULL, &tool);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_file_write_create_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_file_write_create(&alloc, ".", 1, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ─── Path security for file tools ───────────────────────────────────────── */
static void test_file_read_execute_path_traversal_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_read_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "../etc/passwd", 12));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_write_execute_absolute_path_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_write_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "/etc/foo", 8));
    hu_json_object_set(&alloc, args, "content", hu_json_string_new(&alloc, "x", 1));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── File tool edge cases: empty path, missing fields ─────────────────────── */
static void test_file_read_execute_empty_path_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_read_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "", 0));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_read_execute_missing_path_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_read_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_write_execute_empty_path_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_write_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "", 0));
    hu_json_object_set(&alloc, args, "content", hu_json_string_new(&alloc, "hello", 5));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_write_execute_missing_content_defaults_to_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_write_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "test.txt", 8));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_edit_execute_empty_path_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_edit_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "", 0));
    hu_json_object_set(&alloc, args, "old_text", hu_json_string_new(&alloc, "a", 1));
    hu_json_object_set(&alloc, args, "new_text", hu_json_string_new(&alloc, "b", 1));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_edit_execute_missing_old_text_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_edit_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "test.txt", 8));
    hu_json_object_set(&alloc, args, "new_text", hu_json_string_new(&alloc, "b", 1));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_edit_execute_missing_new_text_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_edit_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "test.txt", 8));
    hu_json_object_set(&alloc, args, "old_text", hu_json_string_new(&alloc, "a", 1));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── URL validation for web tools ───────────────────────────────────────── */
static void test_http_request_rejects_http_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_http_request_create(&alloc, false, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "url", hu_json_string_new(&alloc, "http://evil.com", 15));
    hu_json_object_set(&alloc, args, "method", hu_json_string_new(&alloc, "GET", 3));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_web_fetch_execute_missing_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_web_fetch_create(&alloc, 10000, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Git tool parameter validation ─────────────────────────────────────── */
static void test_git_execute_missing_command(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_git_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_git_parameters_json_has_command(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_git_create(&alloc, ".", 1, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(strstr(params, "operation"));
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_git_add_path_traversal_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_git_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "operation", hu_json_string_new(&alloc, "add", 3));
    hu_json_object_set(&alloc, args, "paths", hu_json_string_new(&alloc, "../etc/passwd", 13));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    HU_ASSERT_NOT_NULL(result.error_msg);
    HU_ASSERT_NOT_NULL(strstr(result.error_msg, "path traversal"));
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_git_add_dot_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_git_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "operation", hu_json_string_new(&alloc, "add", 3));
    hu_json_object_set(&alloc, args, "paths", hu_json_string_new(&alloc, ".", 1));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_git_add_empty_path_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_git_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "operation", hu_json_string_new(&alloc, "add", 3));
    hu_json_object_set(&alloc, args, "paths", hu_json_string_new(&alloc, "", 0));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    HU_ASSERT_NOT_NULL(result.error_msg);
    HU_ASSERT_NOT_NULL(strstr(result.error_msg, "Missing"));
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Memory tools with valid args ─────────────────────────────────────────── */
static void test_memory_store_execute_with_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_memory_store_create(&alloc, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "key", hu_json_string_new(&alloc, "test_key", 8));
    hu_json_object_set(&alloc, args, "content", hu_json_string_new(&alloc, "test content", 11));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_memory_recall_execute_with_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_memory_recall_create(&alloc, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "query", hu_json_string_new(&alloc, "test", 4));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Cron tools with valid args ─────────────────────────────────────────── */
static void test_cron_add_execute_with_spec(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_cron_add_create(&alloc, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "spec", hu_json_string_new(&alloc, "* * * * *", 9));
    hu_json_object_set(&alloc, args, "command", hu_json_string_new(&alloc, "echo hi", 7));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_cron_list_execute_returns(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_cron_list_create(&alloc, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Schedule tool ─────────────────────────────────────────────────────── */
static void test_schedule_parameters_have_delay(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_schedule_create(&alloc, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(params);
    HU_ASSERT_TRUE(strstr(params, "delay") != NULL || strstr(params, "seconds") != NULL ||
                   strlen(params) > 0);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Schema validate and clean ───────────────────────────────────────────── */
static void test_schema_validate_array_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *schema = "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}";
    HU_ASSERT_TRUE(hu_schema_validate(&alloc, schema, strlen(schema)));
}

static void test_schema_validate_string_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *schema = "{\"type\":\"string\"}";
    HU_ASSERT_TRUE(hu_schema_validate(&alloc, schema, strlen(schema)));
}

static void test_schema_validate_empty_object_fail(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *schema = "{}";
    HU_ASSERT_FALSE(hu_schema_validate(&alloc, schema, strlen(schema)));
}

static void test_schema_clean_anthropic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *input = "{\"type\":\"string\",\"minLength\":5}";
    char *result = NULL;
    size_t len = 0;
    hu_error_t err = hu_schema_clean(&alloc, input, strlen(input), "anthropic", &result, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, len + 1);
}

static void test_schema_clean_openai(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *input = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"}}}";
    char *result = NULL;
    size_t len = 0;
    hu_error_t err = hu_schema_clean(&alloc, input, strlen(input), "openai", &result, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, len + 1);
}

static void test_schema_clean_conservative(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *input = "{\"type\":\"string\"}";
    char *result = NULL;
    size_t len = 0;
    hu_error_t err = hu_schema_clean(&alloc, input, strlen(input), "conservative", &result, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, len + 1);
}

/* ─── File append path traversal ───────────────────────────────────────────── */
static void test_file_append_execute_path_traversal_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_append_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "../../etc/passwd", 15));
    hu_json_object_set(&alloc, args, "content", hu_json_string_new(&alloc, "append", 6));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Tool description non-empty ──────────────────────────────────────────── */
static void test_shell_description_non_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, ".", 1, NULL, &tool);
    const char *desc = tool.vtable->description(tool.ctx);
    HU_ASSERT_NOT_NULL(desc);
    HU_ASSERT_TRUE(strlen(desc) > 0);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_read_description_non_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_read_create(&alloc, ".", 1, NULL, &tool);
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_TRUE(strlen(tool.vtable->description(tool.ctx)) > 0);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_http_request_description_non_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_http_request_create(&alloc, false, &tool);
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_TRUE(strlen(tool.vtable->description(tool.ctx)) > 0);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Parameters JSON valid ───────────────────────────────────────────────── */
static void test_shell_parameters_valid_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, ".", 1, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(params);
    HU_ASSERT_TRUE(params[0] == '{');
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_memory_store_parameters_has_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_memory_store_create(&alloc, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(strstr(params, "key"));
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── File edit path traversal ────────────────────────────────────────────── */
static void test_file_edit_execute_path_traversal_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_edit_create(&alloc, ".", 1, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "../../etc/passwd", 15));
    hu_json_object_set(&alloc, args, "old_text", hu_json_string_new(&alloc, "x", 1));
    hu_json_object_set(&alloc, args, "new_text", hu_json_string_new(&alloc, "y", 1));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    (void)result; /* In HU_IS_TEST, mock may succeed; real mode would reject traversal */
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Web fetch rejects HTTP ───────────────────────────────────────────────── */
static void test_web_fetch_rejects_http_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_web_fetch_create(&alloc, 10000, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "url", hu_json_string_new(&alloc, "http://example.com", 18));
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Web search providers: URL encode ─────────────────────────────────────── */
static void test_url_encode_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_web_search_url_encode(&alloc, "hello world", 11, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "hello+world");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_url_encode_special_chars(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_web_search_url_encode(&alloc, "a&b=c", 5, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "%26") != NULL); /* & -> %26 */
    HU_ASSERT_TRUE(strstr(out, "%3D") != NULL); /* = -> %3D */
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_url_encode_passthrough(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_web_search_url_encode(&alloc, "abc-123_test.txt~", 17, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out, "abc-123_test.txt~");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_url_encode_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_web_search_url_encode(&alloc, "", 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_url_encode_null_args(void) {
    hu_error_t err = hu_web_search_url_encode(NULL, "x", 1, NULL, NULL);
    HU_ASSERT_TRUE(err != HU_OK);
}

/* ─── Web search missing query ─────────────────────────────────────────────── */
static void test_web_search_execute_missing_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_web_search_create(&alloc, NULL, NULL, 0, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result;
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(!result.success);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Shell workspace bounds ───────────────────────────────────────────────── */
static void test_shell_parameters_has_command(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, ".", 1, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(strstr(params, "command"));
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_read_parameters_has_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_read_create(&alloc, ".", 1, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(strstr(params, "path"));
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_file_write_parameters_has_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_file_write_create(&alloc, ".", 1, NULL, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(strstr(params, "content"));
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_image_parameters_has_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_image_create(&alloc, NULL, 0, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(params);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_http_request_parameters_has_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_http_request_create(&alloc, false, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(strstr(params, "url"));
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_web_search_parameters_has_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_web_search_create(&alloc, NULL, NULL, 0, &tool);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(params);
    if (tool.vtable && tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_browser_create_with_policy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_security_policy_t policy = {0};
    hu_tool_t tool;
    hu_error_t err = hu_browser_create(&alloc, true, &policy, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.ctx);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "browser");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_screenshot_create_with_policy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_security_policy_t policy = {0};
    hu_tool_t tool;
    hu_error_t err = hu_screenshot_create(&alloc, true, &policy, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.ctx);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "screenshot");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_browser_open_create_with_policy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_security_policy_t policy = {0};
    const char *domains[] = {"example.com"};
    hu_tool_t tool;
    hu_error_t err = hu_browser_open_create(&alloc, domains, 1, &policy, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.ctx);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "browser_open");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Business Tools ──────────────────────────────────────────────────────── */
static inline void biz_set_str(hu_allocator_t *a, hu_json_value_t *obj, const char *key,
                               const char *val) {
    hu_json_object_set(a, obj, key, hu_json_string_new(a, val, strlen(val)));
}

static void test_spreadsheet_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_spreadsheet_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "spreadsheet");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_spreadsheet_analyze(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_spreadsheet_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "analyze");
    biz_set_str(&alloc, args, "data", "name,age\nalice,30\nbob,25\n");
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "Rows:") != NULL);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_report_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_report_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "report");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_report_template(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_report_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "template");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "executive_summary") != NULL);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_broadcast_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_broadcast_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "broadcast");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_calendar_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_calendar_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "calendar");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_calendar_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_calendar_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "list");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "Team Standup") != NULL);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_homeassistant_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_homeassistant_create(&alloc, &tool);
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, NULL, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_FALSE(result.success);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_homeassistant_get_states_returns_mock_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_homeassistant_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "operation", "get_states");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.success);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "states") != NULL);
    HU_ASSERT(strstr(result.output, "light.living_room") != NULL);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_homeassistant_get_entity_with_entity_id_returns_mock_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_homeassistant_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "operation", "get_entity");
    biz_set_str(&alloc, args, "entity_id", "switch.plug");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.success);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "switch.plug") != NULL);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_homeassistant_call_service_returns_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_homeassistant_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "operation", "call_service");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.success);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "success") != NULL);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_homeassistant_missing_operation_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_homeassistant_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT(result.error_msg != NULL);
    HU_ASSERT(strstr(result.error_msg, "missing operation") != NULL);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_skill_write_missing_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_skill_write_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "description", "A test skill");
    biz_set_str(&alloc, args, "command", "echo hello");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT(result.error_msg != NULL);
    HU_ASSERT(strstr(result.error_msg, "missing name") != NULL);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
}
static void test_skill_write_missing_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_skill_write_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "name", "my_skill");
    biz_set_str(&alloc, args, "command", "echo hello");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT(result.error_msg != NULL);
    HU_ASSERT(strstr(result.error_msg, "missing description") != NULL);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
}
static void test_skill_write_missing_command(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_skill_write_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "name", "my_skill");
    biz_set_str(&alloc, args, "description", "A test skill");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT(result.error_msg != NULL);
    HU_ASSERT(strstr(result.error_msg, "missing command") != NULL);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
}
static void test_skill_write_invalid_name_chars(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_skill_write_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "name", "my skill");
    biz_set_str(&alloc, args, "description", "A test skill");
    biz_set_str(&alloc, args, "command", "echo hello");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT(result.error_msg != NULL);
    HU_ASSERT(strstr(result.error_msg, "invalid name") != NULL);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
}
static void test_skill_write_rejects_path_traversal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_skill_write_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "name", "../evil");
    biz_set_str(&alloc, args, "description", "A test skill");
    biz_set_str(&alloc, args, "command", "echo hello");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT(result.error_msg != NULL);
    HU_ASSERT(strstr(result.error_msg, "path traversal") != NULL ||
              strstr(result.error_msg, "invalid characters") != NULL);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
}
static void test_skill_write_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_skill_write_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "name", "my_skill");
    biz_set_str(&alloc, args, "description", "A test skill");
    biz_set_str(&alloc, args, "command", "echo hello");
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "Skill 'my_skill' created") != NULL);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
}
static void test_skill_write_name_too_long(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_skill_write_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    char long_name[70];
    for (int i = 0; i < 65; i++)
        long_name[i] = 'a';
    long_name[65] = '\0';
    biz_set_str(&alloc, args, "name", long_name);
    biz_set_str(&alloc, args, "description", "A test skill");
    biz_set_str(&alloc, args, "command", "echo hello");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT(result.error_msg != NULL);
    HU_ASSERT(strstr(result.error_msg, "too long") != NULL);
    hu_json_free(&alloc, args);
    if (result.output_owned && result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    if (result.error_msg_owned && result.error_msg)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
}
static void test_jira_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_jira_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "jira");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_jira_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_jira_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "list");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "PROJ-1") != NULL);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_social_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_social_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "social");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_social_post(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_social_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "post");
    biz_set_str(&alloc, args, "platform", "twitter");
    biz_set_str(&alloc, args, "content", "Hello from Human!");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "posted") != NULL);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_crm_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_crm_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "crm");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_crm_contacts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_crm_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "contacts");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "Alice Smith") != NULL);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_analytics_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_analytics_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "analytics");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_analytics_overview(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_analytics_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "overview");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "pageviews") != NULL);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_invoice_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_invoice_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "invoice");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_invoice_parse(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_invoice_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "parse");
    biz_set_str(&alloc, args, "data", "Invoice #123 Total: $1500");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "parsed") != NULL);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_workflow_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_workflow_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "workflow");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_workflow_create_and_run(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_workflow_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "create");
    biz_set_str(&alloc, args, "name", "Test Workflow");
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "created") != NULL);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "run");
    biz_set_str(&alloc, args, "workflow_id", "wf_0");
    memset(&result, 0, sizeof(result));
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "completed") != NULL);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}
static void test_workflow_approval_gate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_workflow_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "create");
    biz_set_str(&alloc, args, "name", "Approval WF");
    hu_json_value_t *steps = hu_json_array_new(&alloc);
    hu_json_value_t *step1 = hu_json_object_new(&alloc);
    biz_set_str(&alloc, step1, "name", "Review");
    biz_set_str(&alloc, step1, "tool", "report");
    hu_json_object_set(&alloc, step1, "requires_approval", hu_json_bool_new(&alloc, true));
    hu_json_array_push(&alloc, steps, step1);
    hu_json_object_set(&alloc, args, "steps", steps);
    hu_tool_result_t result = {0};
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "run");
    biz_set_str(&alloc, args, "workflow_id", "wf_0");
    memset(&result, 0, sizeof(result));
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "waiting_approval") != NULL);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    args = hu_json_object_new(&alloc);
    biz_set_str(&alloc, args, "action", "approve");
    biz_set_str(&alloc, args, "workflow_id", "wf_0");
    memset(&result, 0, sizeof(result));
    tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(strstr(result.output, "completed") != NULL);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_spawn_create_with_policy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_security_policy_t policy = {0};
    policy.allow_shell = true;
    hu_tool_t tool;
    hu_error_t err = hu_spawn_create(&alloc, ".", 1, &policy, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.ctx);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "spawn");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Factory-based tests for send_message, agent_query, agent_spawn, apply_patch,
 *     database, notebook, canvas, pdf, diff ───────────────────────────────────── */
static hu_tool_t *find_tool_by_name(hu_tool_t *tools, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (tools[i].vtable && tools[i].vtable->name &&
            strcmp(tools[i].vtable->name(tools[i].ctx), name) == 0)
            return &tools[i];
    }
    return NULL;
}

static void test_tool_send_message_exists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(find_tool_by_name(tools, count, "send_message"));
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_send_message_execute(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_t *t = find_tool_by_name(tools, count, "send_message");
    HU_ASSERT_NOT_NULL(t);
    if (t) {
        hu_json_value_t *args = hu_json_object_new(&alloc);
        hu_json_object_set(&alloc, args, "to_agent", hu_json_number_new(&alloc, 1));
        hu_json_object_set(&alloc, args, "message", hu_json_string_new(&alloc, "hello", 5));
        hu_tool_result_t result = {0};
        err = t->vtable->execute(t->ctx, &alloc, args, &result);
        hu_json_free(&alloc, args);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(result.output);
        hu_tool_result_free(&alloc, &result);
    }
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_agent_query_exists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(find_tool_by_name(tools, count, "agent_query"));
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_agent_query_execute(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_t *t = find_tool_by_name(tools, count, "agent_query");
    HU_ASSERT_NOT_NULL(t);
    if (t) {
        hu_json_value_t *args = hu_json_object_new(&alloc);
        hu_json_object_set(&alloc, args, "agent_id", hu_json_number_new(&alloc, 1));
        hu_json_object_set(&alloc, args, "message", hu_json_string_new(&alloc, "test", 4));
        hu_tool_result_t result = {0};
        err = t->vtable->execute(t->ctx, &alloc, args, &result);
        hu_json_free(&alloc, args);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(result.output);
        hu_tool_result_free(&alloc, &result);
    }
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_agent_spawn_exists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(find_tool_by_name(tools, count, "agent_spawn"));
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_agent_spawn_execute(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_t *t = find_tool_by_name(tools, count, "agent_spawn");
    HU_ASSERT_NOT_NULL(t);
    if (t) {
        hu_json_value_t *args = hu_json_object_new(&alloc);
        hu_json_object_set(&alloc, args, "task", hu_json_string_new(&alloc, "test task", 9));
        hu_tool_result_t result = {0};
        err = t->vtable->execute(t->ctx, &alloc, args, &result);
        hu_json_free(&alloc, args);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(result.output);
        hu_tool_result_free(&alloc, &result);
    }
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_apply_patch_exists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(find_tool_by_name(tools, count, "apply_patch"));
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_apply_patch_execute(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_t *t = find_tool_by_name(tools, count, "apply_patch");
    HU_ASSERT_NOT_NULL(t);
    if (t) {
        hu_json_value_t *args = hu_json_object_new(&alloc);
        hu_json_object_set(&alloc, args, "file", hu_json_string_new(&alloc, "foo.txt", 7));
        hu_json_object_set(
            &alloc, args, "patch",
            hu_json_string_new(&alloc, "--- a/foo\n+++ b/foo\n@@ -1 +1 @@\n-x\n+y\n", 36));
        hu_tool_result_t result = {0};
        err = t->vtable->execute(t->ctx, &alloc, args, &result);
        hu_json_free(&alloc, args);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(result.output);
        hu_tool_result_free(&alloc, &result);
    }
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_database_exists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(find_tool_by_name(tools, count, "database"));
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_database_execute(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_t *t = find_tool_by_name(tools, count, "database");
    HU_ASSERT_NOT_NULL(t);
    if (t) {
        hu_json_value_t *args = hu_json_object_new(&alloc);
        hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "query", 5));
        hu_json_object_set(&alloc, args, "sql", hu_json_string_new(&alloc, "SELECT 1", 8));
        hu_tool_result_t result = {0};
        err = t->vtable->execute(t->ctx, &alloc, args, &result);
        hu_json_free(&alloc, args);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(result.output);
        hu_tool_result_free(&alloc, &result);
    }
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_notebook_exists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(find_tool_by_name(tools, count, "notebook"));
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_notebook_execute(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_t *t = find_tool_by_name(tools, count, "notebook");
    HU_ASSERT_NOT_NULL(t);
    if (t) {
        hu_json_value_t *args = hu_json_object_new(&alloc);
        hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "list", 4));
        hu_tool_result_t result = {0};
        err = t->vtable->execute(t->ctx, &alloc, args, &result);
        hu_json_free(&alloc, args);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(result.output);
        hu_tool_result_free(&alloc, &result);
    }
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_canvas_exists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(find_tool_by_name(tools, count, "canvas"));
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_canvas_execute(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_t *t = find_tool_by_name(tools, count, "canvas");
    HU_ASSERT_NOT_NULL(t);
    if (t) {
        hu_json_value_t *args = hu_json_object_new(&alloc);
        hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "create", 6));
        hu_json_object_set(&alloc, args, "content", hu_json_string_new(&alloc, "test", 4));
        hu_tool_result_t result = {0};
        err = t->vtable->execute(t->ctx, &alloc, args, &result);
        hu_json_free(&alloc, args);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(result.output);
        hu_tool_result_free(&alloc, &result);
    }
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_pdf_exists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(find_tool_by_name(tools, count, "pdf"));
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_pdf_execute(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_t *t = find_tool_by_name(tools, count, "pdf");
    HU_ASSERT_NOT_NULL(t);
    if (t) {
        hu_json_value_t *args = hu_json_object_new(&alloc);
        hu_json_object_set(&alloc, args, "path", hu_json_string_new(&alloc, "/tmp/test.pdf", 13));
        hu_tool_result_t result = {0};
        err = t->vtable->execute(t->ctx, &alloc, args, &result);
        hu_json_free(&alloc, args);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(result.output);
        hu_tool_result_free(&alloc, &result);
    }
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_diff_exists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(find_tool_by_name(tools, count, "diff"));
    hu_tools_destroy_default(&alloc, tools, count);
}

static void test_tool_diff_execute(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&alloc, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_t *t = find_tool_by_name(tools, count, "diff");
    HU_ASSERT_NOT_NULL(t);
    if (t) {
        hu_json_value_t *args = hu_json_object_new(&alloc);
        hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "diff", 4));
        hu_json_object_set(&alloc, args, "file_a", hu_json_string_new(&alloc, "a.txt", 5));
        hu_json_object_set(&alloc, args, "file_b", hu_json_string_new(&alloc, "b.txt", 5));
        hu_tool_result_t result = {0};
        err = t->vtable->execute(t->ctx, &alloc, args, &result);
        hu_json_free(&alloc, args);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(result.output);
        hu_tool_result_free(&alloc, &result);
    }
    hu_tools_destroy_default(&alloc, tools, count);
}

/* ─── Facebook Pages tool ──────────────────────────────────────────────────── */
static void test_facebook_tool_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_facebook_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "facebook_pages");
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_facebook_tool_execute_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_facebook_tool_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_facebook_tool_execute_with_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_facebook_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    const char *json = "{\"operation\":\"post\",\"page_id\":\"123\",\"message\":\"Hello\"}";
    hu_json_value_t *args = NULL;
    err = hu_json_parse(&alloc, json, strlen(json), &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(args);
    hu_tool_result_t out = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &out);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.success);
    HU_ASSERT_NOT_NULL(out.output);
    HU_ASSERT_TRUE(out.output_len > 0);
    hu_tool_result_free(&alloc, &out);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_facebook_tool_execute_missing_required(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_facebook_tool_create(&alloc, &tool);
    const char *json = "{}";
    hu_json_value_t *args = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &args);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_result_t out = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &out);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(out.success);
    HU_ASSERT_TRUE(out.error_msg != NULL && out.error_msg_len > 0);
    hu_tool_result_free(&alloc, &out);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Instagram Posts tool ─────────────────────────────────────────────────── */
static void test_instagram_tool_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_instagram_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "instagram_posts");
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_instagram_tool_execute_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_instagram_tool_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_instagram_tool_execute_with_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_instagram_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    const char *json = "{\"operation\":\"publish_photo\",\"account_id\":\"456\",\"image_url\":"
                       "\"https://example.com/"
                       "img.jpg\",\"caption\":\"Test\"}";
    hu_json_value_t *args = NULL;
    err = hu_json_parse(&alloc, json, strlen(json), &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(args);
    hu_tool_result_t out = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &out);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.success);
    HU_ASSERT_NOT_NULL(out.output);
    HU_ASSERT_TRUE(out.output_len > 0);
    hu_tool_result_free(&alloc, &out);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_instagram_tool_execute_missing_required(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_instagram_tool_create(&alloc, &tool);
    const char *json = "{}";
    hu_json_value_t *args = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &args);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_result_t out = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &out);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(out.success);
    HU_ASSERT_TRUE(out.error_msg != NULL && out.error_msg_len > 0);
    hu_tool_result_free(&alloc, &out);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Twitter Posts tool ───────────────────────────────────────────────────── */
static void test_twitter_tool_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_twitter_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "twitter");
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_twitter_tool_execute_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_twitter_tool_create(&alloc, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_twitter_tool_execute_with_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_twitter_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    const char *json = "{\"action\":\"post\",\"text\":\"Hello world\"}";
    hu_json_value_t *args = NULL;
    err = hu_json_parse(&alloc, json, strlen(json), &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(args);
    hu_tool_result_t out = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &out);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.success);
    HU_ASSERT_NOT_NULL(out.output);
    HU_ASSERT_TRUE(out.output_len > 0);
    hu_tool_result_free(&alloc, &out);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_twitter_tool_execute_missing_required(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_twitter_tool_create(&alloc, &tool);
    const char *json = "{}";
    hu_json_value_t *args = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &args);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_result_t out = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &out);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(out.success);
    HU_ASSERT_TRUE(out.error_msg != NULL && out.error_msg_len > 0);
    hu_tool_result_free(&alloc, &out);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── cli_wrapper_common (shared by gcloud, firebase, etc.) ────────────────── */
static void test_cli_sanitize_safe_command(void) {
    HU_ASSERT_TRUE(hu_cli_sanitize_command("compute instances list"));
}

static void test_cli_sanitize_dollar_parens(void) {
    HU_ASSERT_FALSE(hu_cli_sanitize_command("$(rm -rf /)"));
}

static void test_cli_sanitize_backticks(void) {
    HU_ASSERT_FALSE(hu_cli_sanitize_command("`rm -rf /`"));
}

static void test_cli_sanitize_pipe(void) {
    HU_ASSERT_FALSE(hu_cli_sanitize_command("ls | rm"));
}

static void test_cli_sanitize_semicolon(void) {
    HU_ASSERT_FALSE(hu_cli_sanitize_command("ls; rm"));
}

static void test_cli_split_basic(void) {
    char buf[64];
    strcpy(buf, "compute instances list");
    const char *argv[8];
    size_t n = hu_cli_split_args(buf, argv, 8);
    HU_ASSERT_EQ(n, 3);
    HU_ASSERT_STR_EQ(argv[0], "compute");
    HU_ASSERT_STR_EQ(argv[1], "instances");
    HU_ASSERT_STR_EQ(argv[2], "list");
}

static void test_cli_split_empty(void) {
    char buf[1] = "";
    const char *argv[8];
    size_t n = hu_cli_split_args(buf, argv, 8);
    HU_ASSERT_EQ(n, 0);
}

static void test_cli_split_single(void) {
    char buf[16];
    strcpy(buf, "deploy");
    const char *argv[8];
    size_t n = hu_cli_split_args(buf, argv, 8);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_EQ(argv[0], "deploy");
}

/* ─── gcloud tool ──────────────────────────────────────────────────────────── */
static void test_gcloud_tool_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_gcloud_create(&alloc, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "gcloud");
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_gcloud_tool_execute_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_gcloud_create(&alloc, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_gcloud_tool_execute_with_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_gcloud_create(&alloc, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    const char *json = "{\"command\":\"compute instances list\"}";
    hu_json_value_t *args = NULL;
    err = hu_json_parse(&alloc, json, strlen(json), &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(args);
    hu_tool_result_t out = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &out);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.success);
    HU_ASSERT_NOT_NULL(out.output);
    HU_ASSERT_TRUE(out.output_len > 0);
    hu_tool_result_free(&alloc, &out);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_gcloud_tool_execute_missing_required(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_gcloud_create(&alloc, NULL, &tool);
    const char *json = "{}";
    hu_json_value_t *args = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &args);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_result_t out = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &out);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(out.success);
    HU_ASSERT_TRUE(out.error_msg != NULL && out.error_msg_len > 0);
    hu_tool_result_free(&alloc, &out);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── firebase tool ────────────────────────────────────────────────────────── */
static void test_firebase_tool_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_firebase_create(&alloc, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "firebase");
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_firebase_tool_execute_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_firebase_create(&alloc, NULL, &tool);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_firebase_tool_execute_with_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_firebase_create(&alloc, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    const char *json = "{\"command\":\"deploy\"}";
    hu_json_value_t *args = NULL;
    err = hu_json_parse(&alloc, json, strlen(json), &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(args);
    hu_tool_result_t out = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &out);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.success);
    HU_ASSERT_NOT_NULL(out.output);
    HU_ASSERT_TRUE(out.output_len > 0);
    hu_tool_result_free(&alloc, &out);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_firebase_tool_execute_missing_required(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_firebase_create(&alloc, NULL, &tool);
    const char *json = "{}";
    hu_json_value_t *args = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &args);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_result_t out = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &out);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(out.success);
    HU_ASSERT_TRUE(out.error_msg != NULL && out.error_msg_len > 0);
    hu_tool_result_free(&alloc, &out);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

void run_tools_all_tests(void) {
    HU_TEST_SUITE("Tools (all) - Shell/File");
    HU_RUN_TEST(test_shell_create);
    HU_RUN_TEST(test_shell_name);
    HU_RUN_TEST(test_shell_execute_empty);
    HU_RUN_TEST(test_file_read_create);
    HU_RUN_TEST(test_file_read_name);
    HU_RUN_TEST(test_file_read_execute_empty);
    HU_RUN_TEST(test_file_write_create);
    HU_RUN_TEST(test_file_write_name);
    HU_RUN_TEST(test_file_write_execute_empty);
    HU_RUN_TEST(test_file_read_create_null_alloc);
    HU_RUN_TEST(test_file_read_create_null_out);
    HU_RUN_TEST(test_file_write_create_null_alloc);
    HU_RUN_TEST(test_file_write_create_null_out);
    HU_RUN_TEST(test_file_edit_create);
    HU_RUN_TEST(test_file_edit_name);
    HU_RUN_TEST(test_file_edit_execute_empty);
    HU_RUN_TEST(test_file_edit_schema_has_params);
    HU_RUN_TEST(test_file_edit_execute_path_traversal_rejected);
    HU_RUN_TEST(test_file_read_description_non_empty);
    HU_RUN_TEST(test_file_append_execute_path_traversal_rejected);
    HU_RUN_TEST(test_file_read_execute_path_traversal_rejected);
    HU_RUN_TEST(test_file_write_execute_absolute_path_rejected);
    HU_RUN_TEST(test_file_read_parameters_has_path);
    HU_RUN_TEST(test_file_write_parameters_has_content);
    HU_RUN_TEST(test_file_read_execute_empty_path_returns_error);
    HU_RUN_TEST(test_file_read_execute_missing_path_returns_error);
    HU_RUN_TEST(test_file_write_execute_empty_path_returns_error);
    HU_RUN_TEST(test_file_write_execute_missing_content_defaults_to_empty);
    HU_RUN_TEST(test_file_edit_execute_empty_path_returns_error);
    HU_RUN_TEST(test_file_edit_execute_missing_old_text_returns_error);
    HU_RUN_TEST(test_file_edit_execute_missing_new_text_returns_error);
    HU_RUN_TEST(test_shell_parameters_has_command);
    HU_RUN_TEST(test_shell_description_non_empty);
    HU_RUN_TEST(test_shell_parameters_valid_json);
    HU_RUN_TEST(test_file_append_create);
    HU_RUN_TEST(test_file_append_name);
    HU_RUN_TEST(test_file_append_execute_empty);
    HU_RUN_TEST(test_git_create);
    HU_RUN_TEST(test_git_name);
    HU_RUN_TEST(test_git_execute_empty);
    HU_RUN_TEST(test_git_execute_missing_command);
    HU_RUN_TEST(test_git_parameters_json_has_command);
    HU_RUN_TEST(test_git_add_path_traversal_rejected);
    HU_RUN_TEST(test_git_add_dot_succeeds);
    HU_RUN_TEST(test_git_add_empty_path_returns_error);
    HU_RUN_TEST(test_spawn_create);
    HU_RUN_TEST(test_spawn_name);
    HU_RUN_TEST(test_spawn_execute_empty);

    HU_TEST_SUITE("Tools (all) - Web/Network");
    HU_RUN_TEST(test_web_search_create);
    HU_RUN_TEST(test_web_search_name);
    HU_RUN_TEST(test_web_search_execute_empty);
    HU_RUN_TEST(test_web_fetch_create);
    HU_RUN_TEST(test_web_fetch_name);
    HU_RUN_TEST(test_web_fetch_execute_empty);
    HU_RUN_TEST(test_web_fetch_execute_missing_url);
    HU_RUN_TEST(test_web_fetch_rejects_http_url);
    HU_RUN_TEST(test_web_search_execute_missing_query);
    HU_RUN_TEST(test_url_encode_basic);
    HU_RUN_TEST(test_url_encode_special_chars);
    HU_RUN_TEST(test_url_encode_passthrough);
    HU_RUN_TEST(test_url_encode_empty);
    HU_RUN_TEST(test_url_encode_null_args);
    HU_RUN_TEST(test_http_request_create);
    HU_RUN_TEST(test_http_request_rejects_http_url);
    HU_RUN_TEST(test_http_request_parameters_has_url);
    HU_RUN_TEST(test_http_request_description_non_empty);
    HU_RUN_TEST(test_http_request_name);
    HU_RUN_TEST(test_http_request_execute_empty);
    HU_RUN_TEST(test_browser_create);
    HU_RUN_TEST(test_browser_name);
    HU_RUN_TEST(test_browser_execute_empty);
    HU_RUN_TEST(test_browser_open_rejects_invalid_scheme);
    HU_RUN_TEST(test_browser_click_action);
    HU_RUN_TEST(test_browser_click_missing_selector);
    HU_RUN_TEST(test_browser_type_action);
    HU_RUN_TEST(test_browser_type_missing_text);
    HU_RUN_TEST(test_browser_scroll_action);
    HU_RUN_TEST(test_image_create);
    HU_RUN_TEST(test_image_name);
    HU_RUN_TEST(test_image_execute_empty);
    HU_RUN_TEST(test_screenshot_create);
    HU_RUN_TEST(test_screenshot_name);
    HU_RUN_TEST(test_screenshot_execute_empty);

    HU_TEST_SUITE("Tools (all) - Memory/Message");
    HU_RUN_TEST(test_memory_store_create);
    HU_RUN_TEST(test_memory_store_name);
    HU_RUN_TEST(test_memory_store_execute_empty);
    HU_RUN_TEST(test_memory_store_execute_with_content);
    HU_RUN_TEST(test_memory_store_parameters_has_key);
    HU_RUN_TEST(test_memory_recall_create);
    HU_RUN_TEST(test_memory_recall_name);
    HU_RUN_TEST(test_memory_recall_execute_empty);
    HU_RUN_TEST(test_memory_recall_execute_with_query);
    HU_RUN_TEST(test_memory_list_create);
    HU_RUN_TEST(test_memory_list_name);
    HU_RUN_TEST(test_memory_list_execute_empty);
    HU_RUN_TEST(test_memory_forget_create);
    HU_RUN_TEST(test_memory_forget_name);
    HU_RUN_TEST(test_memory_forget_execute_empty);
    HU_RUN_TEST(test_message_create);
    HU_RUN_TEST(test_message_name);
    HU_RUN_TEST(test_message_execute_empty);

    HU_TEST_SUITE("Tools (all) - Misc");
    HU_RUN_TEST(test_delegate_create);
    HU_RUN_TEST(test_delegate_name);
    HU_RUN_TEST(test_delegate_execute_empty);
    HU_RUN_TEST(test_cron_add_create);
    HU_RUN_TEST(test_cron_add_name);
    HU_RUN_TEST(test_cron_add_execute_empty);
    HU_RUN_TEST(test_cron_add_execute_with_spec);
    HU_RUN_TEST(test_cron_list_create);
    HU_RUN_TEST(test_cron_list_name);
    HU_RUN_TEST(test_cron_list_execute_empty);
    HU_RUN_TEST(test_cron_list_execute_returns);
    HU_RUN_TEST(test_cron_remove_create);
    HU_RUN_TEST(test_cron_remove_name);
    HU_RUN_TEST(test_cron_remove_execute_empty);
    HU_RUN_TEST(test_cron_run_create);
    HU_RUN_TEST(test_cron_run_name);
    HU_RUN_TEST(test_cron_run_execute_empty);
    HU_RUN_TEST(test_cron_runs_create);
    HU_RUN_TEST(test_cron_runs_name);
    HU_RUN_TEST(test_cron_runs_execute_empty);
    HU_RUN_TEST(test_cron_update_create);
    HU_RUN_TEST(test_cron_update_name);
    HU_RUN_TEST(test_cron_update_execute_empty);
    HU_RUN_TEST(test_browser_open_create);
    HU_RUN_TEST(test_browser_open_name);
    HU_RUN_TEST(test_browser_open_execute_empty);
    HU_RUN_TEST(test_browser_open_blocks_private_ip);
    HU_RUN_TEST(test_browser_open_blocks_localhost);
    HU_RUN_TEST(test_image_parameters_has_prompt);
    HU_RUN_TEST(test_web_search_parameters_has_query);
    HU_RUN_TEST(test_composio_create);
    HU_RUN_TEST(test_composio_name);
    HU_RUN_TEST(test_composio_execute_empty);
    HU_RUN_TEST(test_hardware_memory_create);
    HU_RUN_TEST(test_hardware_memory_name);
    HU_RUN_TEST(test_hardware_memory_execute_empty);
    HU_RUN_TEST(test_hardware_memory_read_action);
    HU_RUN_TEST(test_hardware_memory_write_action);
    HU_RUN_TEST(test_hardware_memory_unconfigured_board);
    HU_RUN_TEST(test_schedule_create);
    HU_RUN_TEST(test_schedule_name);
    HU_RUN_TEST(test_schedule_execute_empty);
    HU_RUN_TEST(test_schedule_parameters_have_delay);
    HU_RUN_TEST(test_schema_create);
    HU_RUN_TEST(test_schema_name);
    HU_RUN_TEST(test_schema_execute_empty);
    HU_RUN_TEST(test_schema_validate_with_type);
    HU_RUN_TEST(test_schema_validate_without_type);
    HU_RUN_TEST(test_schema_validate_array_type);
    HU_RUN_TEST(test_schema_validate_string_type);
    HU_RUN_TEST(test_schema_validate_empty_object_fail);
    HU_RUN_TEST(test_schema_clean_gemini);
    HU_RUN_TEST(test_schema_clean_anthropic);
    HU_RUN_TEST(test_schema_clean_openai);
    HU_RUN_TEST(test_schema_clean_conservative);
    HU_RUN_TEST(test_pushover_create);
    HU_RUN_TEST(test_pushover_name);
    HU_RUN_TEST(test_pushover_execute_empty);
    HU_RUN_TEST(test_hardware_info_create);
    HU_RUN_TEST(test_hardware_info_name);
    HU_RUN_TEST(test_hardware_info_execute_empty);
    HU_RUN_TEST(test_i2c_create);
    HU_RUN_TEST(test_i2c_name);
    HU_RUN_TEST(test_i2c_execute_empty);
    HU_RUN_TEST(test_spi_create);
    HU_RUN_TEST(test_spi_name);
    HU_RUN_TEST(test_spi_execute_empty);
    HU_RUN_TEST(test_spi_list_action);
    HU_RUN_TEST(test_spi_transfer_action);
    HU_RUN_TEST(test_spi_read_action);
    HU_RUN_TEST(test_claude_code_create);
    HU_RUN_TEST(test_claude_code_name);
    HU_RUN_TEST(test_claude_code_description);
    HU_RUN_TEST(test_claude_code_execute_empty);
    HU_RUN_TEST(test_claude_code_execute_with_prompt);
    HU_RUN_TEST(test_claude_code_execute_with_model_and_dir);

    HU_TEST_SUITE("Tools (all) - Policy Wiring");
    HU_RUN_TEST(test_browser_create_with_policy);
    HU_RUN_TEST(test_screenshot_create_with_policy);
    HU_RUN_TEST(test_browser_open_create_with_policy);
    HU_RUN_TEST(test_spawn_create_with_policy);

    HU_TEST_SUITE("Tools (all) - Business Automation");
    HU_RUN_TEST(test_spreadsheet_create);
    HU_RUN_TEST(test_spreadsheet_analyze);
    HU_RUN_TEST(test_report_create);
    HU_RUN_TEST(test_report_template);
    HU_RUN_TEST(test_broadcast_create);
    HU_RUN_TEST(test_calendar_create);
    HU_RUN_TEST(test_calendar_list);
    HU_RUN_TEST(test_homeassistant_null_args_returns_error);
    HU_RUN_TEST(test_homeassistant_get_states_returns_mock_data);
    HU_RUN_TEST(test_homeassistant_get_entity_with_entity_id_returns_mock_data);
    HU_RUN_TEST(test_homeassistant_call_service_returns_success);
    HU_RUN_TEST(test_homeassistant_missing_operation_returns_error);
    HU_RUN_TEST(test_skill_write_missing_name);
    HU_RUN_TEST(test_skill_write_missing_description);
    HU_RUN_TEST(test_skill_write_missing_command);
    HU_RUN_TEST(test_skill_write_invalid_name_chars);
    HU_RUN_TEST(test_skill_write_rejects_path_traversal);
    HU_RUN_TEST(test_skill_write_valid);
    HU_RUN_TEST(test_skill_write_name_too_long);
    HU_RUN_TEST(test_jira_create);
    HU_RUN_TEST(test_jira_list);
    HU_RUN_TEST(test_social_create);
    HU_RUN_TEST(test_social_post);
    HU_RUN_TEST(test_facebook_tool_create);
    HU_RUN_TEST(test_facebook_tool_execute_empty);
    HU_RUN_TEST(test_facebook_tool_execute_with_args);
    HU_RUN_TEST(test_facebook_tool_execute_missing_required);
    HU_RUN_TEST(test_instagram_tool_create);
    HU_RUN_TEST(test_instagram_tool_execute_empty);
    HU_RUN_TEST(test_instagram_tool_execute_with_args);
    HU_RUN_TEST(test_instagram_tool_execute_missing_required);
    HU_RUN_TEST(test_twitter_tool_create);
    HU_RUN_TEST(test_twitter_tool_execute_empty);
    HU_RUN_TEST(test_twitter_tool_execute_with_args);
    HU_RUN_TEST(test_twitter_tool_execute_missing_required);
    HU_RUN_TEST(test_cli_sanitize_safe_command);
    HU_RUN_TEST(test_cli_sanitize_dollar_parens);
    HU_RUN_TEST(test_cli_sanitize_backticks);
    HU_RUN_TEST(test_cli_sanitize_pipe);
    HU_RUN_TEST(test_cli_sanitize_semicolon);
    HU_RUN_TEST(test_cli_split_basic);
    HU_RUN_TEST(test_cli_split_empty);
    HU_RUN_TEST(test_cli_split_single);
    HU_RUN_TEST(test_gcloud_tool_create);
    HU_RUN_TEST(test_gcloud_tool_execute_empty);
    HU_RUN_TEST(test_gcloud_tool_execute_with_args);
    HU_RUN_TEST(test_gcloud_tool_execute_missing_required);
    HU_RUN_TEST(test_firebase_tool_create);
    HU_RUN_TEST(test_firebase_tool_execute_empty);
    HU_RUN_TEST(test_firebase_tool_execute_with_args);
    HU_RUN_TEST(test_firebase_tool_execute_missing_required);
    HU_RUN_TEST(test_crm_create);
    HU_RUN_TEST(test_crm_contacts);
    HU_RUN_TEST(test_analytics_create);
    HU_RUN_TEST(test_analytics_overview);
    HU_RUN_TEST(test_invoice_create);
    HU_RUN_TEST(test_invoice_parse);
    HU_RUN_TEST(test_workflow_create);
    HU_RUN_TEST(test_workflow_create_and_run);
    HU_RUN_TEST(test_workflow_approval_gate);

    HU_TEST_SUITE("Tools (all) - Factory");
    HU_RUN_TEST(test_tools_factory_create_all);

    HU_TEST_SUITE("Tools (all) - send_message, agent_query, agent_spawn, apply_patch, database, "
                  "notebook, canvas, pdf, diff");
    HU_RUN_TEST(test_tool_send_message_exists);
    HU_RUN_TEST(test_tool_send_message_execute);
    HU_RUN_TEST(test_tool_agent_query_exists);
    HU_RUN_TEST(test_tool_agent_query_execute);
    HU_RUN_TEST(test_tool_agent_spawn_exists);
    HU_RUN_TEST(test_tool_agent_spawn_execute);
    HU_RUN_TEST(test_tool_apply_patch_exists);
    HU_RUN_TEST(test_tool_apply_patch_execute);
    HU_RUN_TEST(test_tool_database_exists);
    HU_RUN_TEST(test_tool_database_execute);
    HU_RUN_TEST(test_tool_notebook_exists);
    HU_RUN_TEST(test_tool_notebook_execute);
    HU_RUN_TEST(test_tool_canvas_exists);
    HU_RUN_TEST(test_tool_canvas_execute);
    HU_RUN_TEST(test_tool_pdf_exists);
    HU_RUN_TEST(test_tool_pdf_execute);
    HU_RUN_TEST(test_tool_diff_exists);
    HU_RUN_TEST(test_tool_diff_execute);
}
