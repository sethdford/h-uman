#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor/check.h"

/* ────────────────────────────────────────────────────────────────────
 * Registry initialization and basic operations
 * ──────────────────────────────────────────────────────────────────── */

static void test_registry_init_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_doctor_registry_t *r = NULL;

    hu_error_t err = hu_doctor_registry_init(&alloc, &r);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT(r != NULL);

    /* Run with empty cap to verify count is 0 */
    hu_doctor_check_result_t results[10] = {0};
    size_t count = 0;
    err = hu_doctor_registry_run_all(r, NULL, results, &count, 10);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)count, 0);

    hu_doctor_registry_free(r);
}

/* Simple fake check for testing */
static hu_doctor_check_result_t run_fake_pass(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    (void)ctx;
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, "pass", NULL};
}

static hu_doctor_check_result_t run_fake_fail(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    (void)ctx;
    return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, "fail reason", NULL};
}

static hu_doctor_check_result_t run_fake_na(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    (void)ctx;
    return (hu_doctor_check_result_t){HU_DOCTOR_NA, "", NULL};
}

static void test_registry_register_and_iter_in_order(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_doctor_registry_t *r = NULL;

    hu_error_t err = hu_doctor_registry_init(&alloc, &r);
    HU_ASSERT_EQ((int)err, (int)HU_OK);

    /* Register 3 fake checks */
    hu_doctor_check_t check1 = {"check_one", "First check", run_fake_pass, NULL, NULL};
    hu_doctor_check_t check2 = {"check_two", "Second check", run_fake_fail, NULL, NULL};
    hu_doctor_check_t check3 = {"check_three", "Third check", run_fake_na, NULL, NULL};

    err = hu_doctor_registry_register(r, &check1);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    err = hu_doctor_registry_register(r, &check2);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    err = hu_doctor_registry_register(r, &check3);
    HU_ASSERT_EQ((int)err, (int)HU_OK);

    /* Run all checks and verify results come in order */
    hu_doctor_check_result_t results[10] = {0};
    size_t count = 0;
    err = hu_doctor_registry_run_all(r, NULL, results, &count, 10);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)count, 3);

    /* Verify verdicts in order */
    HU_ASSERT_EQ((int)results[0].verdict, (int)HU_DOCTOR_PASS);
    HU_ASSERT_EQ((int)results[1].verdict, (int)HU_DOCTOR_FAIL);
    HU_ASSERT_EQ((int)results[2].verdict, (int)HU_DOCTOR_NA);

    /* Verify reason strings */
    HU_ASSERT(results[0].reason != NULL);
    HU_ASSERT(results[1].reason != NULL);

    hu_doctor_registry_free(r);
}

static void test_registry_run_all_calls_each_check_once(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_doctor_registry_t *r = NULL;

    hu_error_t err = hu_doctor_registry_init(&alloc, &r);
    HU_ASSERT_EQ((int)err, (int)HU_OK);

    /* Register 3 checks */
    hu_doctor_check_t checks[3] = {
        {"c1", "Check 1", run_fake_pass, NULL, NULL},
        {"c2", "Check 2", run_fake_pass, NULL, NULL},
        {"c3", "Check 3", run_fake_pass, NULL, NULL},
    };

    for (int i = 0; i < 3; i++) {
        err = hu_doctor_registry_register(r, &checks[i]);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
    }

    /* Run all checks */
    hu_doctor_check_result_t results[10] = {0};
    size_t count = 0;
    err = hu_doctor_registry_run_all(r, NULL, results, &count, 10);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)count, 3);

    /* Verify all passed */
    for (int i = 0; i < 3; i++) {
        HU_ASSERT_EQ((int)results[i].verdict, (int)HU_DOCTOR_PASS);
    }

    hu_doctor_registry_free(r);
}

static void test_registry_invalid_arguments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_doctor_registry_t *r = NULL;

    /* init with NULL alloc */
    hu_error_t err = hu_doctor_registry_init(NULL, &r);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);

    /* init with NULL out */
    err = hu_doctor_registry_init(&alloc, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);

    /* Valid init */
    err = hu_doctor_registry_init(&alloc, &r);
    HU_ASSERT_EQ((int)err, (int)HU_OK);

    /* register with NULL registry */
    hu_doctor_check_t check = {"test", "Test", run_fake_pass, NULL, NULL};
    err = hu_doctor_registry_register(NULL, &check);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);

    /* register with NULL check */
    err = hu_doctor_registry_register(r, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);

    hu_doctor_registry_free(r);
}

/* ────────────────────────────────────────────────────────────────────
 * Test runner
 * ──────────────────────────────────────────────────────────────────── */

void run_doctor_registry_tests(void) {
    HU_TEST_SUITE("doctor_registry");
    HU_RUN_TEST(test_registry_init_returns_empty);
    HU_RUN_TEST(test_registry_register_and_iter_in_order);
    HU_RUN_TEST(test_registry_run_all_calls_each_check_once);
    HU_RUN_TEST(test_registry_invalid_arguments);
}
