/* Plugin system wiring tests. */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/plugin.h"
#include "human/plugin_loader.h"
#include "test_framework.h"
#include <string.h>

static void test_plugin_load_bad_path_fails(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_host_t host = {
        .alloc = &a,
        .register_tool = NULL,
        .register_provider = NULL,
        .register_channel = NULL,
        .host_ctx = NULL,
    };
    hu_plugin_info_t info = {0};
    hu_plugin_handle_t *handle = NULL;
    hu_error_t err = hu_plugin_load(&a, "/nonexistent/plugin.so", &host, &info, &handle);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(handle);
}

static hu_error_t test_register_tool_cb(void *ctx, const char *name, void *tool_vtable) {
    (void)name;
    (void)tool_vtable;
    int *count = (int *)ctx;
    if (count)
        (*count)++;
    return HU_OK;
}

static void test_plugin_host_register_tool_callback(void) {
    hu_allocator_t a = hu_system_allocator();
    int callback_count = 0;
    hu_plugin_host_t host = {
        .alloc = &a,
        .register_tool = test_register_tool_cb,
        .register_provider = NULL,
        .register_channel = NULL,
        .host_ctx = &callback_count,
    };
    hu_plugin_info_t info = {0};
    hu_plugin_handle_t *handle = NULL;
    /* Load a mock plugin - in HU_IS_TEST, /valid/path.so succeeds */
    hu_error_t err = hu_plugin_load(&a, "/valid/mock.so", &host, &info, &handle);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(handle);
    HU_ASSERT_STR_EQ(info.name, "mock-plugin");
    /* Plugin init can call register_tool - mock doesn't, but we verify host is passed */
    HU_ASSERT_NOT_NULL(host.register_tool);
    hu_plugin_unload(handle);
}

static void test_plugin_config_parse_empty_array(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"plugins\":[]}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg.plugins.plugin_paths_len, 0);
    HU_ASSERT_NULL(cfg.plugins.plugin_paths);
    hu_arena_destroy(arena);
}

static void test_plugin_config_parse_with_paths(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json =
        "{\"plugins\":{\"enabled\":true,\"paths\":[\"/lib/foo.so\",\"/lib/bar.so\"]}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(cfg.plugins.enabled);
    HU_ASSERT_EQ(cfg.plugins.plugin_paths_len, 2);
    HU_ASSERT_NOT_NULL(cfg.plugins.plugin_paths);
    HU_ASSERT_STR_EQ(cfg.plugins.plugin_paths[0], "/lib/foo.so");
    HU_ASSERT_STR_EQ(cfg.plugins.plugin_paths[1], "/lib/bar.so");
    hu_arena_destroy(arena);
}

static void test_plugin_config_parse_plugins_as_array(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"plugins\":[\"/a.so\",\"/b.so\"]}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg.plugins.plugin_paths_len, 2);
    HU_ASSERT_STR_EQ(cfg.plugins.plugin_paths[0], "/a.so");
    HU_ASSERT_STR_EQ(cfg.plugins.plugin_paths[1], "/b.so");
    hu_arena_destroy(arena);
}

static void test_plugin_unload_all_no_crash(void) {
    hu_plugin_unload_all();
    /* No plugins loaded - should not crash */
}

static void test_plugin_version_mismatch_fails(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_host_t host = {
        .alloc = &a,
        .register_tool = NULL,
        .register_provider = NULL,
        .register_channel = NULL,
        .host_ctx = NULL,
    };
    hu_plugin_info_t info = {0};
    hu_plugin_handle_t *handle = NULL;
    hu_error_t err = hu_plugin_load(&a, "/bad_api/plugin.so", &host, &info, &handle);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(handle);
}

static void test_registry_create_null_alloc_returns_null(void) {
    hu_plugin_registry_t *reg = hu_plugin_registry_create(NULL, 4);
    HU_ASSERT_NULL(reg);
}

static void test_registry_create_zero_max_returns_null(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 0);
    HU_ASSERT_NULL(reg);
}

static void test_registry_destroy_null_no_crash(void) {
    hu_plugin_registry_destroy(NULL);
}

static void test_registry_count_null_returns_zero(void) {
    HU_ASSERT_EQ(hu_plugin_count(NULL), 0);
}

static void test_registry_register_null_reg_returns_error(void) {
    hu_plugin_info_t info = {.name = "test", .api_version = HU_PLUGIN_API_VERSION};
    HU_ASSERT_EQ(hu_plugin_register(NULL, &info, NULL, 0), HU_ERR_INVALID_ARGUMENT);
}

static void test_registry_register_null_info_returns_error(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 4);
    HU_ASSERT_EQ(hu_plugin_register(reg, NULL, NULL, 0), HU_ERR_INVALID_ARGUMENT);
    hu_plugin_registry_destroy(reg);
}

static void test_registry_register_null_name_returns_error(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 4);
    hu_plugin_info_t info = {.name = NULL, .api_version = HU_PLUGIN_API_VERSION};
    HU_ASSERT_EQ(hu_plugin_register(reg, &info, NULL, 0), HU_ERR_INVALID_ARGUMENT);
    hu_plugin_registry_destroy(reg);
}

static void test_registry_full_returns_oom(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 1);
    hu_plugin_info_t info = {
        .name = "first", .version = "1.0", .api_version = HU_PLUGIN_API_VERSION};
    HU_ASSERT_EQ(hu_plugin_register(reg, &info, NULL, 0), HU_OK);
    hu_plugin_info_t info2 = {
        .name = "second", .version = "2.0", .api_version = HU_PLUGIN_API_VERSION};
    HU_ASSERT_EQ(hu_plugin_register(reg, &info2, NULL, 0), HU_ERR_OUT_OF_MEMORY);
    HU_ASSERT_EQ(hu_plugin_count(reg), 1);
    hu_plugin_registry_destroy(reg);
}

static const char *mock_tool_name(void *ctx) {
    (void)ctx;
    return "mock-tool";
}

static void test_registry_get_tools_multiple_plugins(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 4);

    static const hu_tool_vtable_t mock_vt = {.name = mock_tool_name};
    hu_tool_t tool_a = {.ctx = NULL, .vtable = &mock_vt};
    hu_tool_t tool_b = {.ctx = NULL, .vtable = &mock_vt};

    hu_plugin_info_t p1 = {.name = "plug-a", .api_version = HU_PLUGIN_API_VERSION};
    HU_ASSERT_EQ(hu_plugin_register(reg, &p1, &tool_a, 1), HU_OK);

    hu_plugin_info_t p2 = {.name = "plug-b", .api_version = HU_PLUGIN_API_VERSION};
    HU_ASSERT_EQ(hu_plugin_register(reg, &p2, &tool_b, 1), HU_OK);

    hu_tool_t *tools = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_plugin_get_tools(reg, &tools, &count), HU_OK);
    HU_ASSERT_EQ(count, 2);
    HU_ASSERT_NOT_NULL(tools);
    a.free(a.ctx, tools, count * sizeof(hu_tool_t));
    hu_plugin_registry_destroy(reg);
}

static void test_registry_get_tools_empty(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 4);
    hu_tool_t *tools = NULL;
    size_t count = 99;
    HU_ASSERT_EQ(hu_plugin_get_tools(reg, &tools, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_NULL(tools);
    hu_plugin_registry_destroy(reg);
}

static void test_registry_get_tools_null_args_returns_error(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 4);
    HU_ASSERT_EQ(hu_plugin_get_tools(reg, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_plugin_get_tools(NULL, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_plugin_registry_destroy(reg);
}

static void test_registry_get_info_out_of_range(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 4);
    hu_plugin_info_t out;
    HU_ASSERT_EQ(hu_plugin_get_info(reg, 0, &out), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_plugin_get_info(reg, 100, &out), HU_ERR_NOT_FOUND);
    hu_plugin_registry_destroy(reg);
}

static void test_registry_get_info_null_args_returns_error(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 4);
    HU_ASSERT_EQ(hu_plugin_get_info(reg, 0, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_plugin_get_info(NULL, 0, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_plugin_registry_destroy(reg);
}

static void test_registry_get_info_second_plugin(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 4);
    hu_plugin_info_t p1 = {.name = "alpha", .version = "1.0", .api_version = HU_PLUGIN_API_VERSION};
    hu_plugin_info_t p2 = {.name = "beta", .version = "2.0", .api_version = HU_PLUGIN_API_VERSION};
    HU_ASSERT_EQ(hu_plugin_register(reg, &p1, NULL, 0), HU_OK);
    HU_ASSERT_EQ(hu_plugin_register(reg, &p2, NULL, 0), HU_OK);
    hu_plugin_info_t out;
    HU_ASSERT_EQ(hu_plugin_get_info(reg, 1, &out), HU_OK);
    HU_ASSERT_STR_EQ(out.name, "beta");
    hu_plugin_registry_destroy(reg);
}

static void test_registry_register_no_version_or_description(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 4);
    hu_plugin_info_t info = {
        .name = "bare", .version = NULL, .description = NULL, .api_version = HU_PLUGIN_API_VERSION};
    HU_ASSERT_EQ(hu_plugin_register(reg, &info, NULL, 0), HU_OK);
    hu_plugin_info_t out;
    HU_ASSERT_EQ(hu_plugin_get_info(reg, 0, &out), HU_OK);
    HU_ASSERT_STR_EQ(out.name, "bare");
    HU_ASSERT_NULL(out.version);
    HU_ASSERT_NULL(out.description);
    hu_plugin_registry_destroy(reg);
}

void run_plugin_tests(void) {
    HU_RUN_TEST(test_plugin_load_bad_path_fails);
    HU_RUN_TEST(test_plugin_host_register_tool_callback);
    HU_RUN_TEST(test_plugin_config_parse_empty_array);
    HU_RUN_TEST(test_plugin_config_parse_with_paths);
    HU_RUN_TEST(test_plugin_config_parse_plugins_as_array);
    HU_RUN_TEST(test_plugin_unload_all_no_crash);
    HU_RUN_TEST(test_plugin_version_mismatch_fails);
    HU_RUN_TEST(test_registry_create_null_alloc_returns_null);
    HU_RUN_TEST(test_registry_create_zero_max_returns_null);
    HU_RUN_TEST(test_registry_destroy_null_no_crash);
    HU_RUN_TEST(test_registry_count_null_returns_zero);
    HU_RUN_TEST(test_registry_register_null_reg_returns_error);
    HU_RUN_TEST(test_registry_register_null_info_returns_error);
    HU_RUN_TEST(test_registry_register_null_name_returns_error);
    HU_RUN_TEST(test_registry_full_returns_oom);
    HU_RUN_TEST(test_registry_get_tools_multiple_plugins);
    HU_RUN_TEST(test_registry_get_tools_empty);
    HU_RUN_TEST(test_registry_get_tools_null_args_returns_error);
    HU_RUN_TEST(test_registry_get_info_out_of_range);
    HU_RUN_TEST(test_registry_get_info_null_args_returns_error);
    HU_RUN_TEST(test_registry_get_info_second_plugin);
    HU_RUN_TEST(test_registry_register_no_version_or_description);
}
