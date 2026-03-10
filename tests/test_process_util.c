/* Process util tests (under HU_IS_TEST: stub impl, no real process execution) */
#include "human/core/allocator.h"
#include "human/core/process_util.h"
#include "test_framework.h"

static void test_process_run_stub_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"echo", "hello", NULL};
    hu_run_result_t result;
    hu_error_t err = hu_process_run(&alloc, argv, NULL, 4096, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(result.stdout_buf);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_EQ(result.exit_code, 0);
    hu_run_result_free(&alloc, &result);
}

static void test_process_run_null_args_fail(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_run_result_t result;
    hu_error_t err = hu_process_run(&alloc, NULL, NULL, 4096, &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_process_run_empty_argv0_fail(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {NULL};
    hu_run_result_t result;
    hu_error_t err = hu_process_run(&alloc, argv, NULL, 4096, &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_run_result_free_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_run_result_free(&alloc, NULL);
}

static void test_run_result_free_clears_buffers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"true", NULL};
    hu_run_result_t result;
    hu_error_t err = hu_process_run(&alloc, argv, NULL, 4096, &result);
    HU_ASSERT_EQ(err, HU_OK);
    char *out = result.stdout_buf;
    char *err_buf = result.stderr_buf;
    hu_run_result_free(&alloc, &result);
    HU_ASSERT_NULL(result.stdout_buf);
    HU_ASSERT_NULL(result.stderr_buf);
    (void)out;
    (void)err_buf;
}

void run_process_util_tests(void) {
    HU_TEST_SUITE("Process util");
    HU_RUN_TEST(test_process_run_stub_success);
    HU_RUN_TEST(test_process_run_null_args_fail);
    HU_RUN_TEST(test_process_run_empty_argv0_fail);
    HU_RUN_TEST(test_run_result_free_null_safe);
    HU_RUN_TEST(test_run_result_free_clears_buffers);
}
