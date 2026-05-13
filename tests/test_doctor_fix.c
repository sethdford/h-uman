#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor_fix.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_tests_run = 0;
static int s_tests_passed = 0;

#define RUN(fn)           \
    do {                  \
        s_tests_run++;    \
        fn();             \
        s_tests_passed++; \
    } while (0)

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
    assert(err == HU_OK);
    assert(count == 6); /* state, skills, plugins, personas, config, imessage */
    assert(results != NULL);

    /* All should report fixed in test mode */
    for (size_t i = 0; i < count; i++) {
        assert(results[i].fixed == true);
        assert(results[i].issue != NULL);
        assert(results[i].action_taken != NULL);
    }

    hu_doctor_fix_results_free(&s_alloc, results, count);
}

/* Insight 3 — iMessage recovery is present in the test-mode dispatch and
 * returns a stable "skipped" or "no recovery needed" marker. The real
 * recovery paths (FDA grant deep-link, stale cache clear) live behind the
 * production #if and are exercised by the e2e-imessage-humanness.sh harness;
 * this test just pins the contract that the new fixer is wired into the
 * default dispatch and reports back without errors. */
static void test_fix_imessage_dispatch(void) {
    hu_doctor_fix_result_t out = {0};
    assert(hu_doctor_fix_imessage(&s_alloc, &out) == HU_OK);
    assert(out.fixed == true);
    assert(out.issue != NULL);
    assert(strstr(out.issue, "imessage") != NULL);
    assert(out.action_taken != NULL);
}

static void test_fix_imessage_null_out_param(void) {
    assert(hu_doctor_fix_imessage(&s_alloc, NULL) == HU_ERR_INVALID_ARGUMENT);
}

static void test_fix_null_args(void) {
    hu_doctor_fix_result_t *results = NULL;
    size_t count = 0;
    assert(hu_doctor_fix(NULL, NULL, &results, &count) == HU_ERR_INVALID_ARGUMENT);
    assert(hu_doctor_fix(&s_alloc, NULL, NULL, &count) == HU_ERR_INVALID_ARGUMENT);
    assert(hu_doctor_fix(&s_alloc, NULL, &results, NULL) == HU_ERR_INVALID_ARGUMENT);
}

static void test_fix_state_dir(void) {
    hu_doctor_fix_result_t out = {0};
    assert(hu_doctor_fix_state_dir(&s_alloc, &out) == HU_OK);
    assert(out.fixed == true);
    assert(out.issue != NULL);
}

static void test_fix_skills_dir(void) {
    hu_doctor_fix_result_t out = {0};
    assert(hu_doctor_fix_skills_dir(&s_alloc, &out) == HU_OK);
    assert(out.fixed == true);
}

static void test_fix_plugins_dir(void) {
    hu_doctor_fix_result_t out = {0};
    assert(hu_doctor_fix_plugins_dir(&s_alloc, &out) == HU_OK);
    assert(out.fixed == true);
}

static void test_fix_personas_dir(void) {
    hu_doctor_fix_result_t out = {0};
    assert(hu_doctor_fix_personas_dir(&s_alloc, &out) == HU_OK);
    assert(out.fixed == true);
}

static void test_fix_default_config(void) {
    hu_doctor_fix_result_t out = {0};
    assert(hu_doctor_fix_default_config(&s_alloc, &out) == HU_OK);
    assert(out.fixed == true);
}

static void test_fix_null_out_param(void) {
    assert(hu_doctor_fix_state_dir(&s_alloc, NULL) == HU_ERR_INVALID_ARGUMENT);
    assert(hu_doctor_fix_skills_dir(&s_alloc, NULL) == HU_ERR_INVALID_ARGUMENT);
    assert(hu_doctor_fix_plugins_dir(&s_alloc, NULL) == HU_ERR_INVALID_ARGUMENT);
    assert(hu_doctor_fix_personas_dir(&s_alloc, NULL) == HU_ERR_INVALID_ARGUMENT);
    assert(hu_doctor_fix_default_config(&s_alloc, NULL) == HU_ERR_INVALID_ARGUMENT);
}

static void test_fix_issue_names(void) {
    hu_doctor_fix_result_t *results = NULL;
    size_t count = 0;
    hu_doctor_fix(&s_alloc, NULL, &results, &count);

    assert(strstr(results[0].issue, "state") != NULL);
    assert(strstr(results[1].issue, "skills") != NULL);
    assert(strstr(results[2].issue, "plugins") != NULL);
    assert(strstr(results[3].issue, "personas") != NULL);
    assert(strstr(results[4].issue, "config") != NULL);

    hu_doctor_fix_results_free(&s_alloc, results, count);
}

static void test_results_free_null_safe(void) {
    hu_doctor_fix_results_free(&s_alloc, NULL, 0);
    hu_doctor_fix_results_free(NULL, NULL, 0);
}

int run_doctor_fix_tests(void) {
    s_tests_run = 0;
    s_tests_passed = 0;

    RUN(test_fix_returns_results);
    RUN(test_fix_null_args);
    RUN(test_fix_state_dir);
    RUN(test_fix_skills_dir);
    RUN(test_fix_plugins_dir);
    RUN(test_fix_personas_dir);
    RUN(test_fix_default_config);
    RUN(test_fix_null_out_param);
    RUN(test_fix_issue_names);
    RUN(test_fix_imessage_dispatch);
    RUN(test_fix_imessage_null_out_param);
    RUN(test_results_free_null_safe);

    printf("  doctor_fix: %d/%d passed\n", s_tests_passed, s_tests_run);
    return s_tests_run - s_tests_passed;
}
