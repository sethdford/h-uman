/* tests/test_doctor_outbound_stats.c
 *
 * Sprint 60 — doctor check for outbound pipeline stats. Pins the
 * contract:
 *   1. After pipeline runs, hu_doctor_check_outbound_stats reflects
 *      the recorded counts.
 *   2. JSON detail has the documented shape.
 *   3. Verdict is always PASS (informational).
 *   4. Per-stage and total fields match the snapshot.
 */

#include "test_framework.h"

#include "human/agent/outbound_stats.h"
#include "human/doctor/check_outbound_stats.h"
#include <string.h>

static void test_check_returns_pass_with_detail_json(void) {
    hu_outbound_stats_reset_for_test();
    hu_outbound_stats_record("crosstalk", 3); /* REJECT */
    hu_outbound_stats_record("persona", 2);   /* REGENERATE */
    hu_outbound_stats_record("strip", 0);     /* SEND */

    hu_doctor_check_t check_copy = hu_doctor_check_outbound_stats;
    hu_doctor_check_result_t r = check_copy.run(&check_copy, NULL);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    HU_ASSERT_NOT_NULL(r.reason);
    HU_ASSERT_NOT_NULL(r.detail_json);

    /* Detail JSON should contain the stage entries we recorded. */
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"name\":\"crosstalk\"") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"name\":\"persona\"") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"name\":\"strip\"") != NULL);
    /* And totals. */
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"total_reject\":1") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"total_regenerate\":1") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"total_send\":1") != NULL);

    hu_outbound_stats_reset_for_test();
}

static void test_check_with_empty_stats_still_passes(void) {
    hu_outbound_stats_reset_for_test();

    hu_doctor_check_t check_copy = hu_doctor_check_outbound_stats;
    hu_doctor_check_result_t r = check_copy.run(&check_copy, NULL);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    HU_ASSERT_NOT_NULL(r.detail_json);
    /* All totals zero. */
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"total_send\":0") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"total_reject\":0") != NULL);
}

static void test_render_json_renders_all_stage_entries(void) {
    hu_outbound_stats_snapshot_t snap = {0};
    snap.counts[HU_OUTBOUND_STATS_STAGE_CROSSTALK][3] = 42; /* REJECT */
    snap.counts[HU_OUTBOUND_STATS_STAGE_STRIP][0] = 7;      /* SEND */

    char buf[2048];
    size_t n = hu_doctor_check_outbound_stats_render_json(&snap, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "\"name\":\"strip\",\"send\":7") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"name\":\"crosstalk\",\"send\":0,\"rewrite\":0,"
                                "\"regenerate\":0,\"reject\":42") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"total_send\":7") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"total_reject\":42") != NULL);
}

static void test_render_json_handles_null_args(void) {
    hu_outbound_stats_snapshot_t snap = {0};
    char buf[64];
    HU_ASSERT_EQ(hu_doctor_check_outbound_stats_render_json(NULL, buf, sizeof(buf)), 0u);
    HU_ASSERT_EQ(hu_doctor_check_outbound_stats_render_json(&snap, NULL, 64), 0u);
    HU_ASSERT_EQ(hu_doctor_check_outbound_stats_render_json(&snap, buf, 0), 0u);
}

static void test_render_json_overflow_returns_zero(void) {
    hu_outbound_stats_snapshot_t snap = {0};
    /* 16-byte buffer can't fit even the opening "{"stages":[" — must
     * return 0 (signaling failure) rather than truncating silently. */
    char tiny[16];
    HU_ASSERT_EQ(hu_doctor_check_outbound_stats_render_json(&snap, tiny, sizeof(tiny)), 0u);
}

static void test_check_metadata_matches_spec(void) {
    HU_ASSERT_STR_EQ(hu_doctor_check_outbound_stats.name, "outbound_stats");
    HU_ASSERT_NOT_NULL(hu_doctor_check_outbound_stats.description);
    HU_ASSERT_NOT_NULL(hu_doctor_check_outbound_stats.run);
    /* No autofix — purely observational. */
    HU_ASSERT_NULL(hu_doctor_check_outbound_stats.fix);
}

void run_doctor_outbound_stats_tests(void) {
    HU_TEST_SUITE("doctor_outbound_stats");
    HU_RUN_TEST(test_check_returns_pass_with_detail_json);
    HU_RUN_TEST(test_check_with_empty_stats_still_passes);
    HU_RUN_TEST(test_render_json_renders_all_stage_entries);
    HU_RUN_TEST(test_render_json_handles_null_args);
    HU_RUN_TEST(test_render_json_overflow_returns_zero);
    HU_RUN_TEST(test_check_metadata_matches_spec);
    hu_outbound_stats_reset_for_test();
}
