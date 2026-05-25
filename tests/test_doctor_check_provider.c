/* tests/test_doctor_check_provider.c
 *
 * Sprint 54 US-C3.3 (Phase 1) — Provider smoke check tests.
 *
 * Tests cover:
 *   1. The pure error classifier (every hu_error_t → reason mapping)
 *   2. The reason → kebab-case string mapper
 *   3. The reason → human message renderer
 *   4. The vtable entry's name + run() signature wired correctly
 *   5. The run() path: NA when ctx is NULL, PASS when ctx is non-NULL
 *      (Phase 1 contract; Phase 2 will exercise the factory call)
 *
 * Deferred to Phase 2 (per US-C3.3.md):
 *   - 1-token complete("ok") network smoke
 *   - 5 FAIL-mode tests using mock-provider failure injection
 *   - 10s timeout test under load
 *
 * Test discipline:
 *   - No `// allow-silent-pass` opt-outs (per .claude/rules/tests-that-pin-bugs.md)
 *   - No HU_ASSERT_TRUE(1) tautologies
 *   - Each test name describes the intent the assertion verifies
 */

#include "test_framework.h"

#include "human/core/error.h"
#include "human/doctor/check.h"
#include "human/doctor/check_provider.h"

#include <string.h>

/* ── Classifier (pure function) ───────────────────────────────────── */

static void test_classify_ok_returns_provider_ok(void) {
    HU_ASSERT_EQ((int)hu_doctor_check_provider_classify(HU_OK), (int)HU_DOCTOR_PROVIDER_OK);
}

static void test_classify_invalid_arg_returns_not_configured(void) {
    HU_ASSERT_EQ((int)hu_doctor_check_provider_classify(HU_ERR_INVALID_ARGUMENT),
                 (int)HU_DOCTOR_PROVIDER_NOT_CONFIGURED);
}

static void test_classify_config_not_found_returns_credentials_missing(void) {
    HU_ASSERT_EQ((int)hu_doctor_check_provider_classify(HU_ERR_CONFIG_NOT_FOUND),
                 (int)HU_DOCTOR_PROVIDER_CREDENTIALS_MISSING);
}

static void test_classify_not_found_also_returns_credentials_missing(void) {
    /* The factory may return either HU_ERR_NOT_FOUND or
     * HU_ERR_CONFIG_NOT_FOUND for missing-key cases depending on
     * the provider layer. Both must classify identically. */
    HU_ASSERT_EQ((int)hu_doctor_check_provider_classify(HU_ERR_NOT_FOUND),
                 (int)HU_DOCTOR_PROVIDER_CREDENTIALS_MISSING);
}

static void test_classify_provider_auth_returns_credentials_invalid(void) {
    HU_ASSERT_EQ((int)hu_doctor_check_provider_classify(HU_ERR_PROVIDER_AUTH),
                 (int)HU_DOCTOR_PROVIDER_CREDENTIALS_INVALID);
}

static void test_classify_rate_limited_returns_rate_limited(void) {
    HU_ASSERT_EQ((int)hu_doctor_check_provider_classify(HU_ERR_PROVIDER_RATE_LIMITED),
                 (int)HU_DOCTOR_PROVIDER_RATE_LIMITED);
}

static void test_classify_provider_unavailable_returns_unreachable(void) {
    HU_ASSERT_EQ((int)hu_doctor_check_provider_classify(HU_ERR_PROVIDER_UNAVAILABLE),
                 (int)HU_DOCTOR_PROVIDER_UNREACHABLE);
}

static void test_classify_unknown_error_returns_other(void) {
    /* HU_ERR_TIMEOUT isn't a provider-specific error; should fall
     * into OTHER bucket so doctor reports unmapped gracefully. */
    HU_ASSERT_EQ((int)hu_doctor_check_provider_classify(HU_ERR_TIMEOUT),
                 (int)HU_DOCTOR_PROVIDER_OTHER);
}

/* ── reason_str (stable kebab-case names) ─────────────────────────── */

static void test_reason_str_ok_is_ok(void) {
    HU_ASSERT_STR_EQ(hu_doctor_check_provider_reason_str(HU_DOCTOR_PROVIDER_OK), "ok");
}

static void test_reason_str_credentials_invalid_is_kebab(void) {
    /* Stable name for --json schema consumers (Sprint 50 US-C3.7).
     * The string MUST stay kebab-case forever — changing it breaks
     * any downstream dashboard that grep's for "credentials-invalid". */
    HU_ASSERT_STR_EQ(hu_doctor_check_provider_reason_str(HU_DOCTOR_PROVIDER_CREDENTIALS_INVALID),
                     "credentials-invalid");
}

static void test_reason_str_all_variants_return_non_null(void) {
    /* Defensive: every enum variant including unknown garbage values
     * MUST return a non-NULL string so --json emitters never crash. */
    HU_ASSERT_NOT_NULL(hu_doctor_check_provider_reason_str(HU_DOCTOR_PROVIDER_OK));
    HU_ASSERT_NOT_NULL(hu_doctor_check_provider_reason_str(HU_DOCTOR_PROVIDER_NOT_CONFIGURED));
    HU_ASSERT_NOT_NULL(hu_doctor_check_provider_reason_str(HU_DOCTOR_PROVIDER_CREDENTIALS_MISSING));
    HU_ASSERT_NOT_NULL(hu_doctor_check_provider_reason_str(HU_DOCTOR_PROVIDER_CREDENTIALS_INVALID));
    HU_ASSERT_NOT_NULL(hu_doctor_check_provider_reason_str(HU_DOCTOR_PROVIDER_RATE_LIMITED));
    HU_ASSERT_NOT_NULL(hu_doctor_check_provider_reason_str(HU_DOCTOR_PROVIDER_UNREACHABLE));
    HU_ASSERT_NOT_NULL(hu_doctor_check_provider_reason_str(HU_DOCTOR_PROVIDER_OTHER));
    /* Garbage value (out of enum) — defensive default. */
    HU_ASSERT_NOT_NULL(hu_doctor_check_provider_reason_str((hu_doctor_provider_reason_t)999));
}

/* ── reason_message (human-readable diagnostics) ──────────────────── */

static void test_message_not_configured_mentions_config_path(void) {
    /* The message must tell the user WHERE to add the provider config
     * so they can fix it without grepping source. */
    const char *msg = hu_doctor_check_provider_reason_message(HU_DOCTOR_PROVIDER_NOT_CONFIGURED);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_TRUE(strstr(msg, "config.json") != NULL);
}

static void test_message_credentials_invalid_mentions_auth(void) {
    const char *msg =
        hu_doctor_check_provider_reason_message(HU_DOCTOR_PROVIDER_CREDENTIALS_INVALID);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_TRUE(strstr(msg, "auth") != NULL);
}

static void test_message_ok_is_empty_string(void) {
    /* When the check passes, the reason field is empty — by convention
     * the --json schema will omit it. */
    const char *msg = hu_doctor_check_provider_reason_message(HU_DOCTOR_PROVIDER_OK);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_STR_EQ(msg, "");
}

/* ── Vtable entry wiring ──────────────────────────────────────────── */

extern hu_doctor_check_t hu_doctor_check_provider;

static void test_vtable_has_stable_name(void) {
    /* The name is what --json output uses; it MUST be stable across
     * releases. "provider_smoke" matches the registry registration. */
    HU_ASSERT_STR_EQ(hu_doctor_check_provider.name, "provider_smoke");
}

static void test_vtable_run_function_pointer_is_set(void) {
    HU_ASSERT_NOT_NULL(hu_doctor_check_provider.run);
}

static void test_vtable_fix_is_null_no_autofix(void) {
    /* This check has no autofix — user must edit config manually.
     * Pinning NULL here prevents an accidental fix() that touches
     * user credentials. */
    HU_ASSERT_TRUE(hu_doctor_check_provider.fix == NULL);
}

/* ── run() runtime path (Phase 1 contract) ────────────────────────── */

static void test_run_with_null_ctx_returns_na(void) {
    /* NULL config → NA (not FAIL). Counts as PASS in aggregate per
     * check.h's HU_DOCTOR_NA contract. */
    hu_doctor_check_result_t result = hu_doctor_check_provider.run(&hu_doctor_check_provider, NULL);
    HU_ASSERT_EQ((int)result.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT_NOT_NULL(result.reason);
}

static void test_run_with_non_null_ctx_returns_pass_phase_1(void) {
    /* Phase 1 contract: any non-NULL ctx → PASS. Phase 2 will exercise
     * the actual factory call + smoke. The test is here so Phase 2's
     * regression is caught: when run() starts returning non-PASS for
     * a structurally-valid ctx, this test will fail and force the
     * Phase-2 author to update the contract intentionally. */
    int dummy_ctx_marker = 1; /* not a real hu_config; Phase 1 doesn't deref it */
    hu_doctor_check_result_t result =
        hu_doctor_check_provider.run(&hu_doctor_check_provider, &dummy_ctx_marker);
    HU_ASSERT_EQ((int)result.verdict, (int)HU_DOCTOR_PASS);
}

/* ── runner ───────────────────────────────────────────────────────── */

void run_doctor_check_provider_tests(void) {
    HU_TEST_SUITE("doctor_check_provider");

    HU_RUN_TEST(test_classify_ok_returns_provider_ok);
    HU_RUN_TEST(test_classify_invalid_arg_returns_not_configured);
    HU_RUN_TEST(test_classify_config_not_found_returns_credentials_missing);
    HU_RUN_TEST(test_classify_not_found_also_returns_credentials_missing);
    HU_RUN_TEST(test_classify_provider_auth_returns_credentials_invalid);
    HU_RUN_TEST(test_classify_rate_limited_returns_rate_limited);
    HU_RUN_TEST(test_classify_provider_unavailable_returns_unreachable);
    HU_RUN_TEST(test_classify_unknown_error_returns_other);

    HU_RUN_TEST(test_reason_str_ok_is_ok);
    HU_RUN_TEST(test_reason_str_credentials_invalid_is_kebab);
    HU_RUN_TEST(test_reason_str_all_variants_return_non_null);

    HU_RUN_TEST(test_message_not_configured_mentions_config_path);
    HU_RUN_TEST(test_message_credentials_invalid_mentions_auth);
    HU_RUN_TEST(test_message_ok_is_empty_string);

    HU_RUN_TEST(test_vtable_has_stable_name);
    HU_RUN_TEST(test_vtable_run_function_pointer_is_set);
    HU_RUN_TEST(test_vtable_fix_is_null_no_autofix);

    HU_RUN_TEST(test_run_with_null_ctx_returns_na);
    HU_RUN_TEST(test_run_with_non_null_ctx_returns_pass_phase_1);
}
