/* Tests for tool_search tool and workspace context detection */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/tool.h"
#include "human/tools/tool_search.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HU_IS_TEST
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * Tool Search Tests
 * ────────────────────────────────────────────────────────────────────────── */

/* Mock tool implementations for testing */
static const char *mock_tool_name_1(void *ctx) {
    (void)ctx;
    return "file_read";
}

static const char *mock_tool_desc_1(void *ctx) {
    (void)ctx;
    return "Read files from the workspace";
}

static const char *mock_tool_params_1(void *ctx) {
    (void)ctx;
    return "{}";
}

static hu_error_t mock_tool_execute_1(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                      hu_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = hu_tool_result_ok("", 0);
    return HU_OK;
}

static const hu_tool_vtable_t mock_vtable_1 = {
    .name = mock_tool_name_1,
    .description = mock_tool_desc_1,
    .parameters_json = mock_tool_params_1,
    .execute = mock_tool_execute_1,
    .deinit = NULL,
};

static const char *mock_tool_name_2(void *ctx) {
    (void)ctx;
    return "web_search";
}

static const char *mock_tool_desc_2(void *ctx) {
    (void)ctx;
    return "Search the web for information";
}

static const char *mock_tool_params_2(void *ctx) {
    (void)ctx;
    return "{}";
}

static hu_error_t mock_tool_execute_2(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                      hu_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = hu_tool_result_ok("", 0);
    return HU_OK;
}

static const hu_tool_vtable_t mock_vtable_2 = {
    .name = mock_tool_name_2,
    .description = mock_tool_desc_2,
    .parameters_json = mock_tool_params_2,
    .execute = mock_tool_execute_2,
    .deinit = NULL,
};

static const char *mock_tool_name_3(void *ctx) {
    (void)ctx;
    return "shell";
}

static const char *mock_tool_desc_3(void *ctx) {
    (void)ctx;
    return "Execute shell commands";
}

static const char *mock_tool_params_3(void *ctx) {
    (void)ctx;
    return "{}";
}

static hu_error_t mock_tool_execute_3(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                      hu_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = hu_tool_result_ok("", 0);
    return HU_OK;
}

static const hu_tool_vtable_t mock_vtable_3 = {
    .name = mock_tool_name_3,
    .description = mock_tool_desc_3,
    .parameters_json = mock_tool_params_3,
    .execute = mock_tool_execute_3,
    .deinit = NULL,
};

static void test_tool_search_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[3];

    /* Create mock tools */
    tools[0].ctx = NULL;
    tools[0].vtable = &mock_vtable_1;
    tools[1].ctx = NULL;
    tools[1].vtable = &mock_vtable_2;
    tools[2].ctx = NULL;
    tools[2].vtable = &mock_vtable_3;

    hu_tool_t search_tool;
    hu_error_t err = hu_tool_search_create(&alloc, tools, 3, &search_tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(search_tool.ctx);
    HU_ASSERT_NOT_NULL(search_tool.vtable);

    if (search_tool.vtable->deinit)
        search_tool.vtable->deinit(search_tool.ctx, &alloc);
}

static void test_tool_search_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[1];
    tools[0].ctx = NULL;
    tools[0].vtable = &mock_vtable_1;

    hu_tool_t search_tool;
    hu_error_t err = hu_tool_search_create(&alloc, tools, 1, &search_tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *name = search_tool.vtable->name(search_tool.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_STR_EQ(name, "tool_search");

    if (search_tool.vtable->deinit)
        search_tool.vtable->deinit(search_tool.ctx, &alloc);
}

static void test_tool_search_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[1];
    tools[0].ctx = NULL;
    tools[0].vtable = &mock_vtable_1;

    hu_tool_t search_tool;
    hu_error_t err = hu_tool_search_create(&alloc, tools, 1, &search_tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *desc = search_tool.vtable->description(search_tool.ctx);
    HU_ASSERT_NOT_NULL(desc);
    HU_ASSERT_STR_EQ(desc, "Search available tools by name or keyword");

    if (search_tool.vtable->deinit)
        search_tool.vtable->deinit(search_tool.ctx, &alloc);
}

static void test_tool_search_parameters_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[1];
    tools[0].ctx = NULL;
    tools[0].vtable = &mock_vtable_1;

    hu_tool_t search_tool;
    hu_error_t err = hu_tool_search_create(&alloc, tools, 1, &search_tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *params = search_tool.vtable->parameters_json(search_tool.ctx);
    HU_ASSERT_NOT_NULL(params);
    HU_ASSERT_STR_CONTAINS(params, "query");
    HU_ASSERT_STR_CONTAINS(params, "string");

    if (search_tool.vtable->deinit)
        search_tool.vtable->deinit(search_tool.ctx, &alloc);
}

static void test_tool_search_execute_empty_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_tool_t search_tool;
    hu_error_t err = hu_tool_search_create(&alloc, NULL, 0, &search_tool);
    HU_ASSERT_EQ(err, HU_OK);

    /* Create a minimal JSON object with query field */
    hu_json_value_t query_val = {
        .type = HU_JSON_STRING,
        .data.string = {.ptr = "file", .len = 4},
    };

    hu_json_pair_t pairs[1] = {
        {.key = "query", .key_len = 5, .value = &query_val},
    };

    hu_json_value_t args = {
        .type = HU_JSON_OBJECT,
        .data.object = {.pairs = pairs, .len = 1, .cap = 1},
    };

    hu_tool_result_t result;
    err = search_tool.vtable->execute(search_tool.ctx, &alloc, &args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_STR_EQ(result.output, "[]");

    hu_tool_result_free(&alloc, &result);

    if (search_tool.vtable->deinit)
        search_tool.vtable->deinit(search_tool.ctx, &alloc);
}

static void test_tool_search_execute_match_by_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[3];

    tools[0].ctx = NULL;
    tools[0].vtable = &mock_vtable_1;
    tools[1].ctx = NULL;
    tools[1].vtable = &mock_vtable_2;
    tools[2].ctx = NULL;
    tools[2].vtable = &mock_vtable_3;

    hu_tool_t search_tool;
    hu_error_t err = hu_tool_search_create(&alloc, tools, 3, &search_tool);
    HU_ASSERT_EQ(err, HU_OK);

    /* Search for "web" — should match web_search */
    hu_json_value_t query_val = {
        .type = HU_JSON_STRING,
        .data.string = {.ptr = "web", .len = 3},
    };

    hu_json_pair_t pairs_web[1] = {
        {.key = "query", .key_len = 5, .value = &query_val},
    };

    hu_json_value_t args = {
        .type = HU_JSON_OBJECT,
        .data.object = {.pairs = pairs_web, .len = 1, .cap = 1},
    };

    hu_tool_result_t result;
    err = search_tool.vtable->execute(search_tool.ctx, &alloc, &args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_STR_CONTAINS(result.output, "web_search");
    HU_ASSERT_STR_CONTAINS(result.output, "Search the web");

    hu_tool_result_free(&alloc, &result);

    if (search_tool.vtable->deinit)
        search_tool.vtable->deinit(search_tool.ctx, &alloc);
}

static void test_tool_search_execute_match_by_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[3];

    tools[0].ctx = NULL;
    tools[0].vtable = &mock_vtable_1;
    tools[1].ctx = NULL;
    tools[1].vtable = &mock_vtable_2;
    tools[2].ctx = NULL;
    tools[2].vtable = &mock_vtable_3;

    hu_tool_t search_tool;
    hu_error_t err = hu_tool_search_create(&alloc, tools, 3, &search_tool);
    HU_ASSERT_EQ(err, HU_OK);

    /* Search for "command" — should match shell (Execute shell commands) */
    hu_json_value_t query_val = {
        .type = HU_JSON_STRING,
        .data.string = {.ptr = "command", .len = 7},
    };

    hu_json_pair_t pairs_cmd[1] = {
        {.key = "query", .key_len = 5, .value = &query_val},
    };

    hu_json_value_t args = {
        .type = HU_JSON_OBJECT,
        .data.object = {.pairs = pairs_cmd, .len = 1, .cap = 1},
    };

    hu_tool_result_t result;
    err = search_tool.vtable->execute(search_tool.ctx, &alloc, &args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_STR_CONTAINS(result.output, "shell");

    hu_tool_result_free(&alloc, &result);

    if (search_tool.vtable->deinit)
        search_tool.vtable->deinit(search_tool.ctx, &alloc);
}

static void test_tool_search_case_insensitive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[1];

    tools[0].ctx = NULL;
    tools[0].vtable = &mock_vtable_1;

    hu_tool_t search_tool;
    hu_error_t err = hu_tool_search_create(&alloc, tools, 1, &search_tool);
    HU_ASSERT_EQ(err, HU_OK);

    /* Search for "FILE" (uppercase) — should match file_read (lowercase) */
    hu_json_value_t query_val = {
        .type = HU_JSON_STRING,
        .data.string = {.ptr = "FILE", .len = 4},
    };

    hu_json_pair_t pairs_file[1] = {
        {.key = "query", .key_len = 5, .value = &query_val},
    };

    hu_json_value_t args = {
        .type = HU_JSON_OBJECT,
        .data.object = {.pairs = pairs_file, .len = 1, .cap = 1},
    };

    hu_tool_result_t result;
    err = search_tool.vtable->execute(search_tool.ctx, &alloc, &args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_STR_CONTAINS(result.output, "file_read");

    hu_tool_result_free(&alloc, &result);

    if (search_tool.vtable->deinit)
        search_tool.vtable->deinit(search_tool.ctx, &alloc);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Workspace Context Tests
 * ────────────────────────────────────────────────────────────────────────── */

void run_tool_search_tests(void) {
    HU_TEST_SUITE("tool_search");
    HU_RUN_TEST(test_tool_search_create);
    HU_RUN_TEST(test_tool_search_name);
    HU_RUN_TEST(test_tool_search_description);
    HU_RUN_TEST(test_tool_search_parameters_json);
    HU_RUN_TEST(test_tool_search_execute_empty_tools);
    HU_RUN_TEST(test_tool_search_execute_match_by_name);
    HU_RUN_TEST(test_tool_search_execute_match_by_description);
    HU_RUN_TEST(test_tool_search_case_insensitive);
}
