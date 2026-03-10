/* QMD retrieval: header and types compile; in HU_IS_TEST all logic is compiled out.
 * Functional tests not needed since code is HU_IS_TEST-guarded. */
#include "human/core/allocator.h"
#include "human/memory/retrieval/qmd.h"
#include "test_framework.h"
#include <string.h>

static void test_qmd_header_compiles(void) {
    /* Verify header exists and hu_qmd_keyword_candidates is callable. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_entry_t *out = NULL;
    size_t count = 99;
    hu_error_t err = hu_qmd_keyword_candidates(&alloc, "/tmp", 4, "query", 5, 10, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
}

static void test_qmd_keyword_candidates_null_alloc_returns_error(void) {
    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    hu_error_t err = hu_qmd_keyword_candidates(NULL, "/tmp", 4, "query", 5, 10, &out, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(count, 0u);
}

static void test_qmd_keyword_candidates_null_query_returns_ok_in_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_entry_t *out = NULL;
    size_t count = 99;
    hu_error_t err = hu_qmd_keyword_candidates(&alloc, "/tmp", 4, NULL, 0, 10, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
}

static void test_qmd_keyword_candidates_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_qmd_keyword_candidates(&alloc, "/tmp", 4, "query", 5, 10, NULL, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_qmd_keyword_candidates_null_out_count_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_entry_t *out = NULL;
    hu_error_t err = hu_qmd_keyword_candidates(&alloc, "/tmp", 4, "query", 5, 10, &out, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_qmd_keyword_candidates_zero_limit_returns_ok_in_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_entry_t *out = NULL;
    size_t count = 99;
    hu_error_t err = hu_qmd_keyword_candidates(&alloc, "/tmp", 4, "query", 5, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
}

static void test_qmd_keyword_candidates_null_workspace_dir_returns_ok_in_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_entry_t *out = NULL;
    size_t count = 99;
    hu_error_t err = hu_qmd_keyword_candidates(&alloc, NULL, 0, "query", 5, 10, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
}

void run_qmd_tests(void) {
    HU_TEST_SUITE("QMD");
    HU_RUN_TEST(test_qmd_header_compiles);
    HU_RUN_TEST(test_qmd_keyword_candidates_null_alloc_returns_error);
    HU_RUN_TEST(test_qmd_keyword_candidates_null_query_returns_ok_in_test);
    HU_RUN_TEST(test_qmd_keyword_candidates_null_out_returns_error);
    HU_RUN_TEST(test_qmd_keyword_candidates_null_out_count_returns_error);
    HU_RUN_TEST(test_qmd_keyword_candidates_zero_limit_returns_ok_in_test);
    HU_RUN_TEST(test_qmd_keyword_candidates_null_workspace_dir_returns_ok_in_test);
}
