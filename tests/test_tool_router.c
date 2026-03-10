#include "human/agent/tool_router.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/tool.h"
#include "test_framework.h"
#include <string.h>

static const char *mock_name_memory_store(void *ctx) {
    (void)ctx;
    return "memory_store";
}
static const char *mock_desc_memory_store(void *ctx) {
    (void)ctx;
    return "Store a fact in memory";
}
static const char *mock_name_obscure(void *ctx) {
    (void)ctx;
    return "obscure_tool";
}
static const char *mock_desc_obscure(void *ctx) {
    (void)ctx;
    return "Does something obscure";
}
static const char *mock_name_web_lookup(void *ctx) {
    (void)ctx;
    return "web_lookup";
}
static const char *mock_desc_web_lookup(void *ctx) {
    (void)ctx;
    return "Search the web for information";
}
static const char *mock_name_file_scan(void *ctx) {
    (void)ctx;
    return "file_scan";
}
static const char *mock_desc_file_scan(void *ctx) {
    (void)ctx;
    return "Scan file contents";
}

static hu_error_t mock_exec_noop(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                 hu_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = hu_tool_result_ok("ok", 2);
    return HU_OK;
}

static void router_always_includes_core_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const hu_tool_vtable_t vtable_memory = {
        .execute = mock_exec_noop,
        .name = mock_name_memory_store,
        .description = mock_desc_memory_store,
        .parameters_json = NULL,
        .deinit = NULL,
    };
    static const hu_tool_vtable_t vtable_obscure = {
        .execute = mock_exec_noop,
        .name = mock_name_obscure,
        .description = mock_desc_obscure,
        .parameters_json = NULL,
        .deinit = NULL,
    };
    hu_tool_t tools[] = {
        {.ctx = NULL, .vtable = &vtable_memory},
        {.ctx = NULL, .vtable = &vtable_obscure},
    };
    hu_tool_selection_t sel;
    memset(&sel, 0, sizeof(sel));

    HU_ASSERT_EQ(hu_tool_router_select(&alloc, "hello", 5, tools, 2, &sel), HU_OK);
    HU_ASSERT_TRUE(sel.count >= 1);

    bool has_memory_store = false;
    for (size_t i = 0; i < sel.count; i++) {
        size_t idx = sel.indices[i];
        const char *name = tools[idx].vtable->name(tools[idx].ctx);
        if (name && strcmp(name, "memory_store") == 0)
            has_memory_store = true;
    }
    HU_ASSERT_TRUE(has_memory_store);
}

static void router_limits_to_max_selected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const hu_tool_vtable_t vtable = {
        .execute = mock_exec_noop,
        .name = mock_name_obscure,
        .description = mock_desc_obscure,
        .parameters_json = NULL,
        .deinit = NULL,
    };
    hu_tool_t tools[22];
    for (size_t i = 0; i < 22; i++)
        tools[i] = (hu_tool_t){.ctx = NULL, .vtable = &vtable};

    hu_tool_selection_t sel;
    memset(&sel, 0, sizeof(sel));
    HU_ASSERT_EQ(hu_tool_router_select(&alloc, "test message", 12, tools, 22, &sel), HU_OK);
    HU_ASSERT_TRUE(sel.count <= HU_TOOL_ROUTER_MAX_SELECTED);
}

static void router_selects_relevant_by_keyword(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const hu_tool_vtable_t vtable_web = {
        .execute = mock_exec_noop,
        .name = mock_name_web_lookup,
        .description = mock_desc_web_lookup,
        .parameters_json = NULL,
        .deinit = NULL,
    };
    static const hu_tool_vtable_t vtable_file = {
        .execute = mock_exec_noop,
        .name = mock_name_file_scan,
        .description = mock_desc_file_scan,
        .parameters_json = NULL,
        .deinit = NULL,
    };
    hu_tool_t tools[] = {
        {.ctx = NULL, .vtable = &vtable_web},
        {.ctx = NULL, .vtable = &vtable_file},
    };
    hu_tool_selection_t sel;
    memset(&sel, 0, sizeof(sel));

    const char *msg = "search the web for news";
    HU_ASSERT_EQ(hu_tool_router_select(&alloc, msg, strlen(msg), tools, 2, &sel), HU_OK);

    /* web_lookup matches "search","web" - higher score than file_scan which matches little */
    bool has_web = false;
    for (size_t i = 0; i < sel.count; i++) {
        size_t idx = sel.indices[i];
        const char *name = tools[idx].vtable->name(tools[idx].ctx);
        if (name && strcmp(name, "web_lookup") == 0)
            has_web = true;
    }
    HU_ASSERT_TRUE(has_web);
}

static void router_null_tools_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_selection_t sel;
    memset(&sel, 0, sizeof(sel));

    hu_error_t err = hu_tool_router_select(&alloc, "hello", 5, NULL, 0, &sel);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sel.count, 0u);
}

static void router_null_message_still_includes_always_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const hu_tool_vtable_t vtable_memory = {
        .execute = mock_exec_noop,
        .name = mock_name_memory_store,
        .description = mock_desc_memory_store,
        .parameters_json = NULL,
        .deinit = NULL,
    };
    hu_tool_t tools[] = {
        {.ctx = NULL, .vtable = &vtable_memory},
    };
    hu_tool_selection_t sel;
    memset(&sel, 0, sizeof(sel));

    hu_error_t err = hu_tool_router_select(&alloc, NULL, 0, tools, 1, &sel);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(sel.count >= 1u);
}

static void router_empty_message_still_includes_always_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const hu_tool_vtable_t vtable_memory = {
        .execute = mock_exec_noop,
        .name = mock_name_memory_store,
        .description = mock_desc_memory_store,
        .parameters_json = NULL,
        .deinit = NULL,
    };
    hu_tool_t tools[] = {
        {.ctx = NULL, .vtable = &vtable_memory},
    };
    hu_tool_selection_t sel;
    memset(&sel, 0, sizeof(sel));

    hu_error_t err = hu_tool_router_select(&alloc, "", 0, tools, 1, &sel);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(sel.count >= 1u);
}

void run_tool_router_tests(void) {
    HU_TEST_SUITE("tool_router");
    HU_RUN_TEST(router_always_includes_core_tools);
    HU_RUN_TEST(router_limits_to_max_selected);
    HU_RUN_TEST(router_selects_relevant_by_keyword);
    HU_RUN_TEST(router_null_tools_returns_empty);
    HU_RUN_TEST(router_null_message_still_includes_always_tools);
    HU_RUN_TEST(router_empty_message_still_includes_always_tools);
}
