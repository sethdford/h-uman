#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor_fix.h"

static void *test_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void test_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}
static hu_allocator_t s_alloc = {.alloc = test_alloc, .free = test_free};

static void test_fix_returns_results(void) {
    hu_doctor_fix_result_t *results = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_fix(&s_alloc, NULL, &results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 5);
    HU_ASSERT_NOT_NULL(results);

    /* All should report fixed in test mode */
    for (size_t i = 0; i < count; i++) {
        HU_ASSERT(results[i].fixed == true);
        HU_ASSERT_NOT_NULL(results[i].issue);
        HU_ASSERT_NOT_NULL(results[i].action_taken);
    }

    hu_doctor_fix_results_free(&s_alloc, results, count);
}

static void test_fix_null_args(void) {
    hu_doctor_fix_result_t *results = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_doctor_fix(NULL, NULL, &results, &count), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_doctor_fix(&s_alloc, NULL, NULL, &count), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_doctor_fix(&s_alloc, NULL, &results, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_fix_state_dir(void) {
    hu_doctor_fix_result_t out = {0};
    HU_ASSERT_EQ(hu_doctor_fix_state_dir(&s_alloc, &out), HU_OK);
    HU_ASSERT(out.fixed == true);
    HU_ASSERT_NOT_NULL(out.issue);
}

static void test_fix_skills_dir(void) {
    hu_doctor_fix_result_t out = {0};
    HU_ASSERT_EQ(hu_doctor_fix_skills_dir(&s_alloc, &out), HU_OK);
    HU_ASSERT(out.fixed == true);
}

static void test_fix_plugins_dir(void) {
    hu_doctor_fix_result_t out = {0};
    HU_ASSERT_EQ(hu_doctor_fix_plugins_dir(&s_alloc, &out), HU_OK);
    HU_ASSERT(out.fixed == true);
}

static void test_fix_personas_dir(void) {
    hu_doctor_fix_result_t out = {0};
    HU_ASSERT_EQ(hu_doctor_fix_personas_dir(&s_alloc, &out), HU_OK);
    HU_ASSERT(out.fixed == true);
}

static void test_fix_default_config(void) {
    hu_doctor_fix_result_t out = {0};
    HU_ASSERT_EQ(hu_doctor_fix_default_config(&s_alloc, &out), HU_OK);
    HU_ASSERT(out.fixed == true);
}

static void test_fix_null_out_param(void) {
    HU_ASSERT_EQ(hu_doctor_fix_state_dir(&s_alloc, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_doctor_fix_skills_dir(&s_alloc, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_doctor_fix_plugins_dir(&s_alloc, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_doctor_fix_personas_dir(&s_alloc, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_doctor_fix_default_config(&s_alloc, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_fix_issue_names(void) {
    hu_doctor_fix_result_t *results = NULL;
    size_t count = 0;
    hu_doctor_fix(&s_alloc, NULL, &results, &count);

    HU_ASSERT_STR_CONTAINS(results[0].issue, "state");
    HU_ASSERT_STR_CONTAINS(results[1].issue, "skills");
    HU_ASSERT_STR_CONTAINS(results[2].issue, "plugins");
    HU_ASSERT_STR_CONTAINS(results[3].issue, "personas");
    HU_ASSERT_STR_CONTAINS(results[4].issue, "config");

    hu_doctor_fix_results_free(&s_alloc, results, count);
}

static void test_results_free_null_safe(void) {
    hu_doctor_fix_results_free(&s_alloc, NULL, 0);
    hu_doctor_fix_results_free(NULL, NULL, 0);
}

void run_doctor_fix_tests(void) {
    HU_TEST_SUITE("doctor_fix");
    HU_RUN_TEST(test_fix_returns_results);
    HU_RUN_TEST(test_fix_null_args);
    HU_RUN_TEST(test_fix_state_dir);
    HU_RUN_TEST(test_fix_skills_dir);
    HU_RUN_TEST(test_fix_plugins_dir);
    HU_RUN_TEST(test_fix_personas_dir);
    HU_RUN_TEST(test_fix_default_config);
    HU_RUN_TEST(test_fix_null_out_param);
    HU_RUN_TEST(test_fix_issue_names);
    HU_RUN_TEST(test_results_free_null_safe);
}
