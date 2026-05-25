/* tests/test_doctor_exit_codes.c
 *
 * Sprint 54 US-C3.9 (Phase 1) — Doctor exit-code aggregator tests.
 *
 * Tests for the pure function `hu_doctor_compute_exit_code`. The function
 * takes an array of hu_doctor_check_result_t and returns one of the
 * HU_DOCTOR_EXIT_* constants.
 *
 * Phase 1 covers: aggregator logic + constant stability + bug-grade
 * detection. Phase 2 will cover the binary's actual exit() call site
 * (deferred — depends on registry-driven main() rewrite).
 *
 * Test discipline:
 *   - No allow-silent-pass opt-outs.
 *   - Each test asserts a real contract (no HU_ASSERT_TRUE(1) tautologies).
 *   - Stable exit code values are pinned (numeric, not enum-derived) so
 *     any accidental renumbering breaks the build instantly.
 */

#include "test_framework.h"

#include "human/doctor.h"
#include "human/doctor/check.h"

#include <stddef.h>

/* ── Stable numeric constants (pinning the wire contract) ─────────── */

static void test_exit_code_ok_is_zero(void) {
    /* 0 is the POSIX "success" convention. Changing this number breaks
     * every shell script that runs `human doctor && do-next-thing`. */
    HU_ASSERT_EQ(HU_DOCTOR_EXIT_OK, 0);
}

static void test_exit_code_user_action_is_one(void) {
    HU_ASSERT_EQ(HU_DOCTOR_EXIT_USER_ACTION, 1);
}

static void test_exit_code_bug_grade_is_two(void) {
    HU_ASSERT_EQ(HU_DOCTOR_EXIT_BUG_GRADE, 2);
}

static void test_exit_code_crash_is_64(void) {
    /* 64 avoids shell-reserved ranges (127=command-not-found, 130=SIGINT,
     * 255=signal). Changing this would silently break crash-detection
     * alerting. */
    HU_ASSERT_EQ(HU_DOCTOR_EXIT_CRASH, 64);
}

/* ── compute_exit_code (pure function) ────────────────────────────── */

static void test_compute_null_input_returns_ok(void) {
    /* No results to inspect → OK. Doctor-invoked-with-zero-checks is
     * structurally fine; not a failure. */
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(NULL, 0), HU_DOCTOR_EXIT_OK);
}

static void test_compute_zero_count_returns_ok(void) {
    /* Same contract: count=0 → OK regardless of the pointer. */
    hu_doctor_check_result_t dummy[1] = {{HU_DOCTOR_FAIL, "x", NULL}};
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(dummy, 0), HU_DOCTOR_EXIT_OK);
}

static void test_compute_all_pass_returns_ok(void) {
    hu_doctor_check_result_t results[] = {
        {HU_DOCTOR_PASS, "", NULL},
        {HU_DOCTOR_PASS, "", NULL},
        {HU_DOCTOR_PASS, "", NULL},
    };
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(results, 3), HU_DOCTOR_EXIT_OK);
}

static void test_compute_pass_plus_na_returns_ok(void) {
    /* NA counts as PASS in aggregate per check.h's HU_DOCTOR_NA contract. */
    hu_doctor_check_result_t results[] = {
        {HU_DOCTOR_PASS, "", NULL},
        {HU_DOCTOR_NA, "platform-not-applicable", NULL},
        {HU_DOCTOR_PASS, "", NULL},
    };
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(results, 3), HU_DOCTOR_EXIT_OK);
}

static void test_compute_single_fail_no_detail_returns_user_action(void) {
    /* FAIL without bug-grade detail_json defaults to user-action.
     * This is the common case (FDA denied, credentials missing). */
    hu_doctor_check_result_t results[] = {
        {HU_DOCTOR_PASS, "", NULL},
        {HU_DOCTOR_FAIL, "missing", NULL},
    };
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(results, 2), HU_DOCTOR_EXIT_USER_ACTION);
}

static void test_compute_fail_with_bug_category_returns_bug_grade(void) {
    /* FAIL with detail_json containing "category":"bug" → bug-grade. */
    hu_doctor_check_result_t results[] = {
        {HU_DOCTOR_PASS, "", NULL},
        {HU_DOCTOR_FAIL, "config corrupt",
         "{\"category\":\"bug\",\"detail\":\"unparseable JSON at line 42\"}"},
    };
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(results, 2), HU_DOCTOR_EXIT_BUG_GRADE);
}

static void test_compute_fail_with_bug_category_spaced_still_bug_grade(void) {
    /* Whitespace tolerance: "category" : "bug" and "category": "bug"
     * BOTH match. This protects against JSON serializers that style
     * differently. */
    hu_doctor_check_result_t r1[] = {{HU_DOCTOR_FAIL, "x", "{\"category\": \"bug\"}"}};
    hu_doctor_check_result_t r2[] = {{HU_DOCTOR_FAIL, "x", "{\"category\" : \"bug\"}"}};
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(r1, 1), HU_DOCTOR_EXIT_BUG_GRADE);
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(r2, 1), HU_DOCTOR_EXIT_BUG_GRADE);
}

static void test_compute_bug_grade_trumps_user_action(void) {
    /* When BOTH a bug-grade and a user-action fail are present, the
     * exit code reflects the higher severity (bug-grade). This is so
     * an alerting integration triggers the developer-investigation
     * runbook even if some failures are merely user-correctable. */
    hu_doctor_check_result_t results[] = {
        {HU_DOCTOR_FAIL, "credentials_missing", NULL},
        {HU_DOCTOR_FAIL, "binary corrupt", "{\"category\":\"bug\"}"},
        {HU_DOCTOR_FAIL, "fda_denied", NULL},
    };
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(results, 3), HU_DOCTOR_EXIT_BUG_GRADE);
}

static void test_compute_misspelled_category_defaults_to_user_action(void) {
    /* If a check writes `"category":"bug"` with a typo (e.g., "bugs"
     * or "Bug"), it falls back to user-action. The author must use
     * the canonical spelling to opt into bug-grade. This is
     * intentional: it prevents accidental severity inflation. */
    hu_doctor_check_result_t r1[] = {{HU_DOCTOR_FAIL, "x", "{\"category\":\"bugs\"}"}};
    hu_doctor_check_result_t r2[] = {{HU_DOCTOR_FAIL, "x", "{\"category\":\"Bug\"}"}};
    hu_doctor_check_result_t r3[] = {{HU_DOCTOR_FAIL, "x", "{\"severity\":\"bug\"}"}};
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(r1, 1), HU_DOCTOR_EXIT_USER_ACTION);
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(r2, 1), HU_DOCTOR_EXIT_USER_ACTION);
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(r3, 1), HU_DOCTOR_EXIT_USER_ACTION);
}

static void test_compute_unknown_verdict_is_bug_grade(void) {
    /* Data corruption or future enum variant we don't recognize →
     * bug-grade. Conservative; flags the situation for a developer. */
    hu_doctor_check_result_t results[] = {
        {(hu_doctor_verdict_t)999, "garbage", NULL},
    };
    HU_ASSERT_EQ(hu_doctor_compute_exit_code(results, 1), HU_DOCTOR_EXIT_BUG_GRADE);
}

/* ── runner ───────────────────────────────────────────────────────── */

void run_doctor_exit_codes_tests(void) {
    HU_TEST_SUITE("doctor_exit_codes");

    /* Stable numeric constants */
    HU_RUN_TEST(test_exit_code_ok_is_zero);
    HU_RUN_TEST(test_exit_code_user_action_is_one);
    HU_RUN_TEST(test_exit_code_bug_grade_is_two);
    HU_RUN_TEST(test_exit_code_crash_is_64);

    /* Aggregator pure function */
    HU_RUN_TEST(test_compute_null_input_returns_ok);
    HU_RUN_TEST(test_compute_zero_count_returns_ok);
    HU_RUN_TEST(test_compute_all_pass_returns_ok);
    HU_RUN_TEST(test_compute_pass_plus_na_returns_ok);
    HU_RUN_TEST(test_compute_single_fail_no_detail_returns_user_action);
    HU_RUN_TEST(test_compute_fail_with_bug_category_returns_bug_grade);
    HU_RUN_TEST(test_compute_fail_with_bug_category_spaced_still_bug_grade);
    HU_RUN_TEST(test_compute_bug_grade_trumps_user_action);
    HU_RUN_TEST(test_compute_misspelled_category_defaults_to_user_action);
    HU_RUN_TEST(test_compute_unknown_verdict_is_bug_grade);
}
