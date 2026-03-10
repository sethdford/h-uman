#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/paperclip/client.h"
#include "human/tools/paperclip.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

/* ── Client init / deinit ────────────────────────────────────────────── */

static void test_paperclip_client_init_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    HU_ASSERT_EQ(hu_paperclip_client_init(NULL, &alloc), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_paperclip_client_init(&c, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_paperclip_client_init_no_env(void) {
#if defined(__unix__) || defined(__APPLE__)
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    /* Unset required env vars so init fails gracefully */
    unsetenv("PAPERCLIP_API_URL");
    unsetenv("PAPERCLIP_AGENT_ID");
    hu_error_t err = hu_paperclip_client_init(&c, &alloc);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
#endif
}

static void client_init_from_config_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    hu_error_t err = hu_paperclip_client_init_from_config(&c, &alloc, "http://localhost:3100/api",
                                                          "agent-123", "company-456");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(c.api_url);
    HU_ASSERT_NOT_NULL(c.agent_id);
    HU_ASSERT_NOT_NULL(c.company_id);
    HU_ASSERT_STR_EQ(c.api_url, "http://localhost:3100/api");
    HU_ASSERT_STR_EQ(c.agent_id, "agent-123");
    HU_ASSERT_STR_EQ(c.company_id, "company-456");
    hu_paperclip_client_deinit(&c);
}

static void client_init_requires_url_and_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    HU_ASSERT_EQ(hu_paperclip_client_init_from_config(&c, &alloc, NULL, "id", NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_paperclip_client_init_from_config(&c, &alloc, "url", NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void client_init_from_env_requires_vars(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    /* Without env vars set, this should fail or succeed based on environment.
       We test the explicit path instead. */
    hu_error_t err = hu_paperclip_client_init_from_config(&c, &alloc, "http://test:3100/api",
                                                          "test-agent", NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(c.company_id);
    hu_paperclip_client_deinit(&c);
}

static void client_deinit_handles_null(void) {
    hu_paperclip_client_deinit(NULL);
    hu_paperclip_client_t c = {0};
    hu_paperclip_client_deinit(&c);
}

/* ── Task parsing ────────────────────────────────────────────────────── */

static void test_paperclip_list_tasks_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    hu_paperclip_client_init_from_config(&c, &alloc, "http://test/api", "a1", "c1");
    hu_paperclip_task_list_t list = {0};
    HU_ASSERT_EQ(hu_paperclip_list_tasks(NULL, &list), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_paperclip_list_tasks(&c, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_paperclip_client_deinit(&c);
}

static void task_list_empty_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    hu_paperclip_client_init_from_config(&c, &alloc, "http://test/api", "a1", "c1");

    hu_paperclip_task_list_t list = {0};
    hu_error_t err = hu_paperclip_list_tasks(&c, &list);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(list.count, 0u);

    hu_paperclip_task_list_free(&alloc, &list);
    hu_paperclip_client_deinit(&c);
}

static void task_free_handles_null_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_task_t task = {0};
    hu_paperclip_task_free(&alloc, &task);
    hu_paperclip_task_free(NULL, &task);
    hu_paperclip_task_free(&alloc, NULL);
}

static void task_list_free_handles_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_task_list_free(&alloc, NULL);
    hu_paperclip_task_list_t list = {0};
    hu_paperclip_task_list_free(&alloc, &list);
}

/* ── Comment list ────────────────────────────────────────────────────── */

static void comment_list_free_handles_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_comment_list_free(&alloc, NULL);
    hu_paperclip_comment_list_t list = {0};
    hu_paperclip_comment_list_free(&alloc, &list);
}

static void get_comments_returns_ok_in_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    hu_paperclip_client_init_from_config(&c, &alloc, "http://test/api", "a1", "c1");

    hu_paperclip_comment_list_t comments = {0};
    hu_error_t err = hu_paperclip_get_comments(&c, "task-1", &comments);
    HU_ASSERT_EQ(err, HU_OK);

    hu_paperclip_comment_list_free(&alloc, &comments);
    hu_paperclip_client_deinit(&c);
}

/* ── Checkout / update / post ────────────────────────────────────────── */

static void checkout_requires_task_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    hu_paperclip_client_init_from_config(&c, &alloc, "http://test/api", "a1", "c1");
    HU_ASSERT_EQ(hu_paperclip_checkout_task(&c, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_paperclip_client_deinit(&c);
}

static void checkout_ok_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    hu_paperclip_client_init_from_config(&c, &alloc, "http://test/api", "a1", "c1");
    HU_ASSERT_EQ(hu_paperclip_checkout_task(&c, "task-1"), HU_OK);
    hu_paperclip_client_deinit(&c);
}

static void update_task_requires_params(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    hu_paperclip_client_init_from_config(&c, &alloc, "http://test/api", "a1", "c1");
    HU_ASSERT_EQ(hu_paperclip_update_task(&c, NULL, "done"), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_paperclip_update_task(&c, "t1", NULL), HU_ERR_INVALID_ARGUMENT);
    hu_paperclip_client_deinit(&c);
}

static void update_task_ok_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    hu_paperclip_client_init_from_config(&c, &alloc, "http://test/api", "a1", "c1");
    HU_ASSERT_EQ(hu_paperclip_update_task(&c, "task-1", "done"), HU_OK);
    hu_paperclip_client_deinit(&c);
}

static void post_comment_requires_params(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    hu_paperclip_client_init_from_config(&c, &alloc, "http://test/api", "a1", "c1");
    HU_ASSERT_EQ(hu_paperclip_post_comment(&c, NULL, "hi", 2), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_paperclip_post_comment(&c, "t1", NULL, 0), HU_ERR_INVALID_ARGUMENT);
    hu_paperclip_client_deinit(&c);
}

static void post_comment_ok_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_paperclip_client_t c = {0};
    hu_paperclip_client_init_from_config(&c, &alloc, "http://test/api", "a1", "c1");
    HU_ASSERT_EQ(hu_paperclip_post_comment(&c, "task-1", "test comment", 12), HU_OK);
    hu_paperclip_client_deinit(&c);
}

/* ── Tool creation ───────────────────────────────────────────────────── */

static void paperclip_tool_creates_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_paperclip_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.ctx);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "paperclip");
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    tool.vtable->deinit(tool.ctx, &alloc);
}

static void paperclip_tool_requires_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_paperclip_tool_create(&alloc, &tool);

    const char *json = "{}";
    hu_json_value_t *args = NULL;
    hu_json_parse(&alloc, json, strlen(json), &args);

    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT_NOT_NULL(strstr(result.output, "action"));

    if (result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    hu_json_free(&alloc, args);
    tool.vtable->deinit(tool.ctx, &alloc);
}

static void paperclip_tool_unknown_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_paperclip_tool_create(&alloc, &tool);

    const char *json = "{\"action\":\"explode\"}";
    hu_json_value_t *args = NULL;
    hu_json_parse(&alloc, json, strlen(json), &args);

    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT_NOT_NULL(result.output);
    HU_ASSERT_TRUE(strstr(result.output, "Unknown") != NULL ||
                   strstr(result.output, "Missing") != NULL);

    if (result.output)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    hu_json_free(&alloc, args);
    tool.vtable->deinit(tool.ctx, &alloc);
}

/* ── Test registration ───────────────────────────────────────────────── */

void run_paperclip_tests(void) {
    HU_TEST_SUITE("paperclip");

    HU_RUN_TEST(test_paperclip_client_init_null_args);
    HU_RUN_TEST(test_paperclip_client_init_no_env);
    HU_RUN_TEST(client_init_from_config_ok);
    HU_RUN_TEST(client_init_requires_url_and_id);
    HU_RUN_TEST(client_init_from_env_requires_vars);
    HU_RUN_TEST(client_deinit_handles_null);

    HU_RUN_TEST(test_paperclip_list_tasks_null_args);
    HU_RUN_TEST(task_list_empty_returns_ok);
    HU_RUN_TEST(task_free_handles_null_fields);
    HU_RUN_TEST(task_list_free_handles_null);

    HU_RUN_TEST(comment_list_free_handles_null);
    HU_RUN_TEST(get_comments_returns_ok_in_test);

    HU_RUN_TEST(checkout_requires_task_id);
    HU_RUN_TEST(checkout_ok_in_test_mode);
    HU_RUN_TEST(update_task_requires_params);
    HU_RUN_TEST(update_task_ok_in_test_mode);
    HU_RUN_TEST(post_comment_requires_params);
    HU_RUN_TEST(post_comment_ok_in_test_mode);

    HU_RUN_TEST(paperclip_tool_creates_ok);
    HU_RUN_TEST(paperclip_tool_requires_action);
    HU_RUN_TEST(paperclip_tool_unknown_action);
}
