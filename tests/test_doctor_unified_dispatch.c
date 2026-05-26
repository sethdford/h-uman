/* tests/test_doctor_unified_dispatch.c
 *
 * M3 Dispatch — doctor check for unified-dispatch health. Pins the
 * verdict thresholds and the JSON detail shape so future refactors
 * don't silently shift the operator-facing semantics.
 *
 * The check reads process-wide atomics (hu_guard_reject_stats_t) via
 * the snapshot API. We drive those atomics through the public recorder
 * (hu_response_guard_record_g9_retry_outcome) — Sprint 41 #3 wired
 * this so daemon retry sites and tests share the same path.
 *
 * Verdict matrix (per check_unified_dispatch.h):
 *   total == 0                          → NA (no data yet)
 *   total < 50                          → NA (low signal)
 *   rate < 0.50  AND total >= 50        → FAIL
 *   0.50 <= rate < 0.80  AND total >= 50 → NA (middling)
 *   rate >= 0.80 AND total >= 50         → PASS
 */

#include "test_framework.h"

#include "human/agent/response_guard.h"
#include "human/doctor/check.h"
#include "human/doctor/check_unified_dispatch.h"
#include <string.h>

/* Helper: record N (rescued, thrashed, starved) outcomes via the
 * public recorder. The function is the same one daemon retry sites
 * call, so this exercises the production codepath, not a test-only
 * stat-setter. */
static void record_outcomes(uint64_t rescued, uint64_t thrashed, uint64_t starved) {
    for (uint64_t i = 0; i < rescued; i++)
        hu_response_guard_record_g9_retry_outcome(/*retry_succeeded=*/true,
                                                  /*retry_tripped_g9_again=*/false);
    for (uint64_t i = 0; i < thrashed; i++)
        hu_response_guard_record_g9_retry_outcome(/*retry_succeeded=*/true,
                                                  /*retry_tripped_g9_again=*/true);
    for (uint64_t i = 0; i < starved; i++)
        hu_response_guard_record_g9_retry_outcome(/*retry_succeeded=*/false,
                                                  /*retry_tripped_g9_again=*/false);
}

/* ── Verdict cases ──────────────────────────────────────────────── */

static void test_no_data_returns_na(void) {
    hu_guard_reject_stats_reset();
    hu_doctor_check_t check_copy = hu_doctor_check_unified_dispatch;
    hu_doctor_check_result_t r = check_copy.run(&check_copy, NULL);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_NA);
    HU_ASSERT_NOT_NULL(r.reason);
    HU_ASSERT_TRUE(strstr(r.reason, "no G9 retry-outcome data yet") != NULL);
    /* Detail JSON always present, even with no data. */
    HU_ASSERT_NOT_NULL(r.detail_json);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"g9_retry_total\":0") != NULL);
}

static void test_low_signal_returns_na(void) {
    /* 10 outcomes total — below the 50-sample threshold. Rate would
     * be 100% but we can't trust it. */
    hu_guard_reject_stats_reset();
    record_outcomes(10, 0, 0);
    hu_doctor_check_t check_copy = hu_doctor_check_unified_dispatch;
    hu_doctor_check_result_t r = check_copy.run(&check_copy, NULL);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_NA);
    HU_ASSERT_TRUE(strstr(r.reason, "low signal") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"g9_retry_total\":10") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"g9_retry_rescued\":10") != NULL);
}

static void test_healthy_rate_returns_pass(void) {
    /* 90/100 = 90% rescue — above the 80% threshold. */
    hu_guard_reject_stats_reset();
    record_outcomes(90, 5, 5);
    hu_doctor_check_t check_copy = hu_doctor_check_unified_dispatch;
    hu_doctor_check_result_t r = check_copy.run(&check_copy, NULL);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    HU_ASSERT_TRUE(strstr(r.reason, "unified dispatch healthy") != NULL);
    HU_ASSERT_TRUE(strstr(r.reason, "90.0%") != NULL);
}

static void test_middling_rate_returns_na(void) {
    /* 65/100 = 65% rescue — between 50% and 80%. NA with "middling". */
    hu_guard_reject_stats_reset();
    record_outcomes(65, 30, 5);
    hu_doctor_check_t check_copy = hu_doctor_check_unified_dispatch;
    hu_doctor_check_result_t r = check_copy.run(&check_copy, NULL);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_NA);
    HU_ASSERT_TRUE(strstr(r.reason, "partially stuck") != NULL);
    HU_ASSERT_TRUE(strstr(r.reason, "65.0%") != NULL);
}

static void test_low_rate_returns_fail(void) {
    /* 20/100 = 20% rescue — below the 50% threshold. FAIL. */
    hu_guard_reject_stats_reset();
    record_outcomes(20, 70, 10);
    hu_doctor_check_t check_copy = hu_doctor_check_unified_dispatch;
    hu_doctor_check_result_t r = check_copy.run(&check_copy, NULL);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_TRUE(strstr(r.reason, "LoRA likely stuck") != NULL);
    HU_ASSERT_TRUE(strstr(r.reason, "20.0%") != NULL);
}

/* ── Boundary tests — exactly at the thresholds ────────────────── */

static void test_exactly_at_pass_threshold_passes(void) {
    /* 40/50 = exactly 80% rescue, exactly 50 outcomes. Both
     * thresholds inclusive on the healthy side. */
    hu_guard_reject_stats_reset();
    record_outcomes(40, 5, 5);
    hu_doctor_check_t check_copy = hu_doctor_check_unified_dispatch;
    hu_doctor_check_result_t r = check_copy.run(&check_copy, NULL);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
}

static void test_exactly_at_fail_threshold_is_na_not_fail(void) {
    /* 25/50 = exactly 50% — FAIL threshold is strictly less than.
     * 50% lands in the "middling" NA band. The boundary placement
     * matters: an operator at exactly 50% should NOT page; they
     * should watch. */
    hu_guard_reject_stats_reset();
    record_outcomes(25, 20, 5);
    hu_doctor_check_t check_copy = hu_doctor_check_unified_dispatch;
    hu_doctor_check_result_t r = check_copy.run(&check_copy, NULL);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_NA);
}

/* ── Detail JSON shape ────────────────────────────────────────── */

static void test_detail_json_includes_all_detector_counters(void) {
    hu_guard_reject_stats_reset();
    record_outcomes(50, 5, 5);
    hu_doctor_check_t check_copy = hu_doctor_check_unified_dispatch;
    hu_doctor_check_result_t r = check_copy.run(&check_copy, NULL);
    /* The all-detector breakdown lets operators cross-reference
     * with DPO log file size. Pin that all six detector keys exist
     * in the JSON. */
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"semantic_leak\":") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"length_anomaly\":") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"director_echo\":") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"persona_pii_echo\":") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"persona_identity_echo\":") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"naked_discourse_opener\":") != NULL);
}

static void test_check_metadata_matches_registration(void) {
    /* The name + description must match what registry.c registers,
     * otherwise the JSON output and CLI selector get out of sync. */
    HU_ASSERT_STR_EQ(hu_doctor_check_unified_dispatch.name, "unified_dispatch");
    HU_ASSERT_NOT_NULL(hu_doctor_check_unified_dispatch.description);
    HU_ASSERT_TRUE(strstr(hu_doctor_check_unified_dispatch.description, "M3 unified-dispatch") !=
                   NULL);
    HU_ASSERT_NULL(hu_doctor_check_unified_dispatch.fix); /* no autofix */
}

void run_doctor_unified_dispatch_tests(void);
void run_doctor_unified_dispatch_tests(void) {
    HU_TEST_SUITE("doctor_unified_dispatch");
    HU_RUN_TEST(test_no_data_returns_na);
    HU_RUN_TEST(test_low_signal_returns_na);
    HU_RUN_TEST(test_healthy_rate_returns_pass);
    HU_RUN_TEST(test_middling_rate_returns_na);
    HU_RUN_TEST(test_low_rate_returns_fail);
    HU_RUN_TEST(test_exactly_at_pass_threshold_passes);
    HU_RUN_TEST(test_exactly_at_fail_threshold_is_na_not_fail);
    HU_RUN_TEST(test_detail_json_includes_all_detector_counters);
    HU_RUN_TEST(test_check_metadata_matches_registration);
    /* Defensive: leave the global counters in a known state for
     * other suites that may inspect them. */
    hu_guard_reject_stats_reset();
}
