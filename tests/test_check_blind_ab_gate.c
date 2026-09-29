/* tests/test_doctor_blind_ab_gate.c — the gate must vouch for what is served. */
#include "human/doctor/check_ops.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include <stdio.h>
#include <string.h>

static void write_text(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
}

/* 2026-09-02 09:00 local-ish; timestamps below are a few days earlier. */
#define NOW 1788350400LL

static const char *k_home_good =
    "{\"human\":{\"tool\":\"blind_ab/score.py\",\"timestamp\":\"2026-08-29T06:36:49\","
    "\"verdict\":\"PASS\",\"detection\":0.225,\"n\":40,"
    "\"arm\":{\"adapter\":\"/x/adapters/seth-glm-air-v6-orpo-real-20260802-190128\"}}}";
static const char *k_home_no_arm =
    "{\"human\":{\"tool\":\"blind_ab/score.py\",\"timestamp\":\"2026-08-29T06:36:49\","
    "\"verdict\":\"PASS\",\"detection\":0.225,\"n\":40}}";
static const char *k_repo_older =
    "{\"human\":{\"tool\":\"blind_ab/score.py\",\"timestamp\":\"2026-07-26T04:38:02\","
    "\"verdict\":\"PASS\",\"detection\":0.5,\"n\":12}}";

static void test_missing_and_unstamped_fail(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_gate", d, sizeof(d)));
    char hp[600];
    snprintf(hp, sizeof(hp), "%s/home.json", d);
    hu_doctor_blind_ab_gate_ctx_t ctx = {hp, NULL, NULL, NOW, 45};
    hu_doctor_check_t c = hu_doctor_check_blind_ab_gate;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "no human blind-A/B verdict");
    write_text(hp, "{\"human\":{\"verdict\":\"PASS\",\"n\":40}}");
    r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "no `tool` stamp");
    hu_test_rm_rf(d);
}

static void test_disagreeing_files_fail(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_gate", d, sizeof(d)));
    char hp[600], rp[600];
    snprintf(hp, sizeof(hp), "%s/home.json", d);
    snprintf(rp, sizeof(rp), "%s/repo.json", d);
    write_text(hp, k_home_good);
    write_text(rp, k_repo_older);
    hu_doctor_blind_ab_gate_ctx_t ctx = {hp, rp, NULL, NOW, 45};
    hu_doctor_check_t c = hu_doctor_check_blind_ab_gate;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "disagree");
    HU_ASSERT_STR_CONTAINS(r.reason, "n=40");
    HU_ASSERT_STR_CONTAINS(r.reason, "n=12");
    write_text(rp, k_home_good);
    r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    hu_test_rm_rf(d);
}

static void test_served_adapter_must_match_measured_arm(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_gate", d, sizeof(d)));
    char hp[600];
    snprintf(hp, sizeof(hp), "%s/home.json", d);
    write_text(hp, k_home_good);
    hu_doctor_check_t c = hu_doctor_check_blind_ab_gate;
    hu_doctor_blind_ab_gate_ctx_t ok = {
        hp, NULL,
        "/Users/x/.human/training-data/adapters/seth-glm-air-v6-orpo-real-20260802-190128", NOW,
        45};
    hu_doctor_check_result_t r = c.run(&c, &ok);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    HU_ASSERT_STR_CONTAINS(r.reason, "arm=seth-glm-air-v6-orpo-real-20260802-190128");
    hu_doctor_blind_ab_gate_ctx_t other = {hp, NULL, "seth-glm-air-v7-kto", NOW, 45};
    r = c.run(&c, &other);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "has not been human-rated");
    write_text(hp, k_home_no_arm);
    r = c.run(&c, &ok);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "does not record which adapter");
    /* unknown served adapter: an arm-less record is still a valid human verdict */
    hu_doctor_blind_ab_gate_ctx_t unknown = {hp, NULL, NULL, NOW, 45};
    r = c.run(&c, &unknown);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    hu_test_rm_rf(d);
}

static void test_stale_verdict_fails(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_gate", d, sizeof(d)));
    char hp[600];
    snprintf(hp, sizeof(hp), "%s/home.json", d);
    write_text(hp, k_repo_older); /* 2026-07-26: 38 days before NOW */
    hu_doctor_blind_ab_gate_ctx_t ctx = {hp, NULL, NULL, NOW, 30};
    hu_doctor_check_t c = hu_doctor_check_blind_ab_gate;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "days old");
    hu_test_rm_rf(d);
}

static void test_parse_ts_shape(void) {
    HU_ASSERT_EQ((int)hu_doctor_gate_parse_ts(NULL), 0);
    HU_ASSERT_EQ((int)hu_doctor_gate_parse_ts("not a date"), 0);
    int64_t a = hu_doctor_gate_parse_ts("2026-07-29T06:36:49");
    int64_t b = hu_doctor_gate_parse_ts("2026-07-30T06:36:49");
    HU_ASSERT_GT(a, 0);
    HU_ASSERT_EQ((int)(b - a), 86400);
    /* date-only is accepted (time defaults to midnight) */
    HU_ASSERT_GT(hu_doctor_gate_parse_ts("2026-07-29"), 0);
}

void run_doctor_blind_ab_gate_tests(void) {
    HU_TEST_SUITE("doctor_blind_ab_gate");
    HU_RUN_TEST(test_parse_ts_shape);
    HU_RUN_TEST(test_missing_and_unstamped_fail);
    HU_RUN_TEST(test_disagreeing_files_fail);
    HU_RUN_TEST(test_served_adapter_must_match_measured_arm);
    HU_RUN_TEST(test_stale_verdict_fails);
}
