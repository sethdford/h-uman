/* Process util tests (under SC_IS_TEST: stub impl, no real process execution) */
#include "test_framework.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/allocator.h"

static void test_process_run_stub_success(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *argv[] = {"echo", "hello", NULL};
    sc_run_result_t result;
    sc_error_t err = sc_process_run(&alloc, argv, NULL, 4096, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(result.stdout_buf);
    SC_ASSERT_TRUE(result.success);
    SC_ASSERT_EQ(result.exit_code, 0);
    sc_run_result_free(&alloc, &result);
}

static void test_process_run_null_args_fail(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_run_result_t result;
    sc_error_t err = sc_process_run(&alloc, NULL, NULL, 4096, &result);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_process_run_empty_argv0_fail(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *argv[] = {NULL};
    sc_run_result_t result;
    sc_error_t err = sc_process_run(&alloc, argv, NULL, 4096, &result);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_run_result_free_null_safe(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_run_result_free(&alloc, NULL);
}

static void test_run_result_free_clears_buffers(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *argv[] = {"true", NULL};
    sc_run_result_t result;
    sc_error_t err = sc_process_run(&alloc, argv, NULL, 4096, &result);
    SC_ASSERT_EQ(err, SC_OK);
    char *out = result.stdout_buf;
    char *err_buf = result.stderr_buf;
    sc_run_result_free(&alloc, &result);
    SC_ASSERT_NULL(result.stdout_buf);
    SC_ASSERT_NULL(result.stderr_buf);
    (void)out;
    (void)err_buf;
}

void run_process_util_tests(void) {
    SC_TEST_SUITE("Process util");
    SC_RUN_TEST(test_process_run_stub_success);
    SC_RUN_TEST(test_process_run_null_args_fail);
    SC_RUN_TEST(test_process_run_empty_argv0_fail);
    SC_RUN_TEST(test_run_result_free_null_safe);
    SC_RUN_TEST(test_run_result_free_clears_buffers);
}
