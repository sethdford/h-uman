#include "human/tools/declarative.h"
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "test_framework.h"
#include <string.h>

static void declarative_discover_null_args(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_declarative_tool_def_t *defs = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_declarative_tools_discover(NULL, "/tmp", &defs, &count), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_declarative_tools_discover(&a, NULL, &defs, &count), HU_ERR_INVALID_ARGUMENT);
}

static void declarative_discover_test_mode(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_declarative_tool_def_t *defs = NULL;
    size_t count = 99;
    HU_ASSERT_EQ(hu_declarative_tools_discover(&a, "/tmp/nonexistent", &defs, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_NULL(defs);
}

static void declarative_create_and_query(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_declarative_tool_def_t def = {0};
    def.name = "weather";
    def.description = "Get weather";
    def.parameters_json = "{\"type\":\"object\"}";
    def.exec_type = HU_DECL_EXEC_HTTP;
    def.exec_url = "https://api.example.com/weather";
    def.exec_method = "GET";
    hu_tool_t tool;
    HU_ASSERT_EQ(hu_declarative_tool_create(&a, &def, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "weather");
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void declarative_def_free_null(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_declarative_tool_def_free(NULL, &a);
}

static void declarative_transform_happy_path(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_declarative_tool_def_t def = {0};
    def.name = "temperature_converter";
    def.description = "Convert temperature";
    def.parameters_json = "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"}}}";
    def.exec_type = HU_DECL_EXEC_TRANSFORM;
    def.exec_transform = "Converted: {{value}} degrees Celsius";

    hu_tool_t tool;
    HU_ASSERT_EQ(hu_declarative_tool_create(&a, &def, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    HU_ASSERT_EQ(hu_json_parse(&a, "{\"value\": \"25\"}", 16, &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &a, args, &result), HU_OK);
    HU_ASSERT_STR_EQ(result.output, "Converted: 25 degrees Celsius");
    HU_ASSERT(result.success);

    if (result.output)
        a.free(a.ctx, (void *)result.output, strlen(result.output) + 1);
    hu_json_free(&a, args);
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void declarative_transform_missing_field(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_declarative_tool_def_t def = {0};
    def.name = "transform_test";
    def.description = "Test";
    def.parameters_json = "{}";
    def.exec_type = HU_DECL_EXEC_TRANSFORM;
    def.exec_transform = "Missing: {{nonexistent}}";

    hu_tool_t tool;
    HU_ASSERT_EQ(hu_declarative_tool_create(&a, &def, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    HU_ASSERT_EQ(hu_json_parse(&a, "{}", 2, &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &a, args, &result), HU_OK);
    /* Missing fields substitute to empty string */
    HU_ASSERT_STR_EQ(result.output, "Missing: ");

    if (result.output)
        a.free(a.ctx, (void *)result.output, strlen(result.output) + 1);
    hu_json_free(&a, args);
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void declarative_transform_no_placeholders(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_declarative_tool_def_t def = {0};
    def.name = "static_transform";
    def.description = "Static output";
    def.parameters_json = "{}";
    def.exec_type = HU_DECL_EXEC_TRANSFORM;
    def.exec_transform = "Static output, no placeholders";

    hu_tool_t tool;
    HU_ASSERT_EQ(hu_declarative_tool_create(&a, &def, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    HU_ASSERT_EQ(hu_json_parse(&a, "{}", 2, &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &a, args, &result), HU_OK);
    HU_ASSERT_STR_EQ(result.output, "Static output, no placeholders");

    if (result.output)
        a.free(a.ctx, (void *)result.output, strlen(result.output) + 1);
    hu_json_free(&a, args);
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void declarative_transform_missing_template(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_declarative_tool_def_t def = {0};
    def.name = "broken_transform";
    def.description = "Missing template";
    def.parameters_json = "{}";
    def.exec_type = HU_DECL_EXEC_TRANSFORM;
    /* exec_transform is NULL */

    hu_tool_t tool;
    HU_ASSERT_EQ(hu_declarative_tool_create(&a, &def, &tool), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &a, NULL, &result), HU_OK);
    HU_ASSERT_NOT_NULL(result.error_msg);
    HU_ASSERT(!result.success);

    if (result.error_msg_owned && result.error_msg)
        a.free(a.ctx, (void *)result.error_msg, strlen(result.error_msg) + 1);
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void declarative_transform_malicious_template(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_declarative_tool_def_t def = {0};
    def.name = "malicious_transform";
    def.description = "Malicious template";
    def.parameters_json = "{}";
    def.exec_type = HU_DECL_EXEC_TRANSFORM;
    def.exec_transform = "Command: $(evil command)";

    hu_tool_t tool;
    HU_ASSERT_EQ(hu_declarative_tool_create(&a, &def, &tool), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &a, NULL, &result), HU_OK);
    HU_ASSERT_NOT_NULL(result.error_msg);
    HU_ASSERT(!result.success);
    /* Should detect the $(subshell) pattern and reject */

    if (result.error_msg_owned && result.error_msg)
        a.free(a.ctx, (void *)result.error_msg, strlen(result.error_msg) + 1);
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void declarative_chain_not_yet_wired(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_declarative_tool_def_t def = {0};
    def.name = "chained_tool";
    def.description = "Chain test";
    def.parameters_json = "{}";
    def.exec_type = HU_DECL_EXEC_CHAIN;
    def.exec_chain = "other_tool";

    hu_tool_t tool;
    HU_ASSERT_EQ(hu_declarative_tool_create(&a, &def, &tool), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &a, NULL, &result), HU_OK);
    HU_ASSERT_NOT_NULL(result.error_msg);
    HU_ASSERT(!result.success);
    HU_ASSERT(strstr(result.error_msg, "registry injection") != NULL);

    if (result.error_msg_owned && result.error_msg)
        a.free(a.ctx, (void *)result.error_msg, strlen(result.error_msg) + 1);
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

void run_declarative_tools_tests(void) {
    HU_TEST_SUITE("DeclarativeTools");
    HU_RUN_TEST(declarative_discover_null_args);
    HU_RUN_TEST(declarative_discover_test_mode);
    HU_RUN_TEST(declarative_create_and_query);
    HU_RUN_TEST(declarative_def_free_null);
    HU_RUN_TEST(declarative_transform_happy_path);
    HU_RUN_TEST(declarative_transform_missing_field);
    HU_RUN_TEST(declarative_transform_no_placeholders);
    HU_RUN_TEST(declarative_transform_missing_template);
    HU_RUN_TEST(declarative_transform_malicious_template);
    HU_RUN_TEST(declarative_chain_not_yet_wired);
}
