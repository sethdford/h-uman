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

/* ── Sprint 60 follow-up: derived health metrics in detail_json ────── */

/* When the pipeline has handled some traffic, detail_json must expose
 * a derived reject_rate field. Dashboards key on this to chart trend
 * without recomputing from per-stage counters. */
static void test_detail_json_exposes_reject_rate(void) {
    hu_outbound_stats_reset_for_test();
    /* 7 SEND, 3 REJECT → 30% reject rate. Use crosstalk so the rate
     * shows up against a known stage, but the rate is global. */
    for (int i = 0; i < 7; i++)
        hu_outbound_stats_record("crosstalk", 0); /* SEND */
    for (int i = 0; i < 3; i++)
        hu_outbound_stats_record("crosstalk", 3); /* REJECT */

    hu_doctor_check_t cc = hu_doctor_check_outbound_stats;
    hu_doctor_check_result_t r = cc.run(&cc, NULL);
    HU_ASSERT_NOT_NULL(r.detail_json);
    /* Rate is reported as a decimal (0.30) — dashboards parse via JSON,
     * not regex, but the substring presence is the contract. */
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"reject_rate\":0.30") != NULL);
    hu_outbound_stats_reset_for_test();
}

/* Empty pipeline (no records yet) reports reject_rate=0.00 — must NOT
 * divide by zero or report NaN. */
static void test_detail_json_empty_pipeline_reports_zero_rate(void) {
    hu_outbound_stats_reset_for_test();
    hu_doctor_check_t cc = hu_doctor_check_outbound_stats;
    hu_doctor_check_result_t r = cc.run(&cc, NULL);
    HU_ASSERT_NOT_NULL(r.detail_json);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"reject_rate\":0.00") != NULL);
}

/* "healthy": true when reject_rate is below the threshold (default 25%)
 * AND no "other"-bucket counts AND no NaN/divbyzero. */
static void test_detail_json_healthy_true_below_threshold(void) {
    hu_outbound_stats_reset_for_test();
    /* 9 SEND, 1 REJECT → 10% reject — below 25% threshold. */
    for (int i = 0; i < 9; i++)
        hu_outbound_stats_record("crosstalk", 0);
    hu_outbound_stats_record("crosstalk", 3);

    hu_doctor_check_t cc = hu_doctor_check_outbound_stats;
    hu_doctor_check_result_t r = cc.run(&cc, NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"healthy\":true") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"warnings\":[]") != NULL);
    hu_outbound_stats_reset_for_test();
}

/* "healthy": false + warnings:["reject_rate_high"] when reject rate
 * exceeds the 25% threshold AND the sample size is large enough. The
 * sample-size floor (100 sends total) prevents a single REJECT in a
 * fresh deploy from tripping the warning. */
static void test_detail_json_healthy_false_above_threshold(void) {
    hu_outbound_stats_reset_for_test();
    /* 60 SEND, 40 REJECT → 40% reject — above 25%, sample size 100. */
    for (int i = 0; i < 60; i++)
        hu_outbound_stats_record("crosstalk", 0);
    for (int i = 0; i < 40; i++)
        hu_outbound_stats_record("crosstalk", 3);

    hu_doctor_check_t cc = hu_doctor_check_outbound_stats;
    hu_doctor_check_result_t r = cc.run(&cc, NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"healthy\":false") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"reject_rate_high\"") != NULL);
    hu_outbound_stats_reset_for_test();
}

/* Small-sample protection: 100% reject rate with only 5 records must
 * stay healthy=true. Without this guard, a fresh deploy with one
 * adversarial test send would trip the warning permanently. */
static void test_detail_json_small_sample_stays_healthy(void) {
    hu_outbound_stats_reset_for_test();
    /* 0 SEND, 5 REJECT → 100% reject but only 5 records. */
    for (int i = 0; i < 5; i++)
        hu_outbound_stats_record("crosstalk", 3);

    hu_doctor_check_t cc = hu_doctor_check_outbound_stats;
    hu_doctor_check_result_t r = cc.run(&cc, NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"healthy\":true") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"warnings\":[]") != NULL);
    hu_outbound_stats_reset_for_test();
}

/* "other" bucket counts indicate the pipeline emitted a stage name we
 * don't recognize — likely a config drift (new stage added without
 * updating the stats subsystem's name table). Warn explicitly. */
static void test_detail_json_warns_on_other_bucket_counts(void) {
    hu_outbound_stats_reset_for_test();
    /* Healthy traffic on known stages. */
    for (int i = 0; i < 100; i++)
        hu_outbound_stats_record("crosstalk", 0);
    /* Plus one record routed to OTHER — bumps the unknown bucket. */
    hu_outbound_stats_record("definitely_not_a_real_stage", 0);

    hu_doctor_check_t cc = hu_doctor_check_outbound_stats;
    hu_doctor_check_result_t r = cc.run(&cc, NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"unknown_stage_counts\"") != NULL);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"healthy\":false") != NULL);
    hu_outbound_stats_reset_for_test();
}

void run_doctor_outbound_stats_tests(void) {
    HU_TEST_SUITE("doctor_outbound_stats");
    HU_RUN_TEST(test_check_returns_pass_with_detail_json);
    HU_RUN_TEST(test_check_with_empty_stats_still_passes);
    HU_RUN_TEST(test_render_json_renders_all_stage_entries);
    HU_RUN_TEST(test_render_json_handles_null_args);
    HU_RUN_TEST(test_render_json_overflow_returns_zero);
    HU_RUN_TEST(test_check_metadata_matches_spec);
    /* Sprint 60 follow-up: health metrics. */
    HU_RUN_TEST(test_detail_json_exposes_reject_rate);
    HU_RUN_TEST(test_detail_json_empty_pipeline_reports_zero_rate);
    HU_RUN_TEST(test_detail_json_healthy_true_below_threshold);
    HU_RUN_TEST(test_detail_json_healthy_false_above_threshold);
    HU_RUN_TEST(test_detail_json_small_sample_stays_healthy);
    HU_RUN_TEST(test_detail_json_warns_on_other_bucket_counts);
    hu_outbound_stats_reset_for_test();
}
