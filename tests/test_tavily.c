/* Tests for hu_web_search_tavily */
#include "human/core/allocator.h"
#include "human/tools/web_search_providers.h"
#include "test_framework.h"

static void test_tavily_null_alloc(void) {
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(NULL, "query", 5, 1, "key", &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_tavily_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, NULL, 0, 1, "key", &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_tavily_null_api_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, "query", 5, 1, NULL, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_tavily_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_web_search_tavily(&alloc, "query", 5, 1, "key", NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_tavily_zero_query_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, "q", 0, 1, "key", &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_tavily_count_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, "query", 5, 0, "key", &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_tavily_count_over_ten(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, "query", 5, 11, "key", &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

#if HU_IS_TEST
/* In test mode, hu_http_post_json returns HU_OK with mock JSON. The mock has no "results"
 * key, so Tavily returns "No web results found." with output_owned=true. */
static void test_tavily_valid_args_returns_result(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, "test query", 10, 3, "test-key", &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(result.output);
    HU_ASSERT_TRUE(result.output_len > 0);
    hu_tool_result_free(&alloc, &result);
}
#endif

void run_tavily_tests(void) {
    HU_TEST_SUITE("Tavily web search");
    HU_RUN_TEST(test_tavily_null_alloc);
    HU_RUN_TEST(test_tavily_null_query);
    HU_RUN_TEST(test_tavily_null_api_key);
    HU_RUN_TEST(test_tavily_null_out);
    HU_RUN_TEST(test_tavily_zero_query_len);
    HU_RUN_TEST(test_tavily_count_zero);
    HU_RUN_TEST(test_tavily_count_over_ten);
#if HU_IS_TEST
    HU_RUN_TEST(test_tavily_valid_args_returns_result);
#endif
}
