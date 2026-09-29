/* tests/test_doctor_eval_freshness.c — the product gate must be SEEN to run.
 * Contract: no artifact → FAIL; stale artifact → FAIL; fresh archive OR fresh
 * log → PASS; empty files are not artifacts. */
#include "human/doctor/check_ops.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

static void touch(const char *path, const char *content, int64_t mtime) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
    struct utimbuf t = {.actime = (time_t)mtime, .modtime = (time_t)mtime};
    utime(path, &t);
}

static void test_no_artifacts_fails(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_evalfresh", d, sizeof(d)));
    char archive[600], log[600];
    snprintf(archive, sizeof(archive), "%s/archive", d);
    snprintf(log, sizeof(log), "%s/nightly-eval.log", d);
    hu_doctor_eval_freshness_ctx_t ctx = {archive, log, 1000000, 3};
    hu_doctor_check_t c = hu_doctor_check_eval_freshness;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "never run");
    hu_test_rm_rf(d);
}

static void test_stale_archive_fails_fresh_passes(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_evalfresh", d, sizeof(d)));
    char archive[600], f[700], log[600];
    snprintf(archive, sizeof(archive), "%s/archive", d);
    mkdir(archive, 0700);
    snprintf(f, sizeof(f), "%s/eval-fidelity-2026-08-08.json", archive);
    snprintf(log, sizeof(log), "%s/nightly-eval.log", d);
    int64_t now = 1000000;
    touch(f, "{}", now - 10 * 86400);
    HU_ASSERT_EQ((int)(hu_doctor_eval_newest_artifact_unix(archive, log) - (now - 10 * 86400)), 0);
    hu_doctor_eval_freshness_ctx_t ctx = {archive, log, now, 3};
    hu_doctor_check_t c = hu_doctor_check_eval_freshness;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "10.0 days ago");
    HU_ASSERT_STR_CONTAINS(r.detail_json, "\"age_days\":10.0");
    /* 2026-09-04: a fresh nightly-eval.log proves the job RAN, not that it
     * measured — the script appends that line after a crash too. Without a
     * real verdict inside the window this is a FAIL that names the cause. */
    touch(log, "[2026-09-02T04:05:00] === nightly_eval done ===\n", now - 3600);
    r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "NEVER produced a real verdict");
    /* A real verdict archived inside the window turns it green. */
    snprintf(f, sizeof(f), "%s/eval-fidelity-2026-09-04.json", archive);
    touch(f, "{\"verdict\":\"PASS\",\"exit_code\":0}", now - 7200);
    r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    HU_ASSERT_STR_CONTAINS(r.reason, "last real verdict 0.1 days ago");
    hu_test_rm_rf(d);
}

/* The exact 2026-09-04 morning: fidelity DEFERRED, multiturn overwritten by a
 * smoke run, log fresh. Pre-fix: "ran 0.0 days ago" PASS. */
static void test_deferred_and_smoke_artifacts_are_not_verdicts(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_evalfresh", d, sizeof(d)));
    char archive[600], f[700], log[600];
    snprintf(archive, sizeof(archive), "%s/archive", d);
    mkdir(archive, 0700);
    snprintf(log, sizeof(log), "%s/nightly-eval.log", d);
    int64_t now = 1000000;
    touch(log, "[2026-09-04T04:55:14] === nightly_eval done ===\n", now - 600);
    snprintf(f, sizeof(f), "%s/eval-fidelity-2026-09-04.json", archive);
    touch(f, "{\"verdict\":\"DEFERRED\",\"exit_code\":2,\"reason\":\"generation failing\"}",
          now - 700);
    snprintf(f, sizeof(f), "%s/eval-multiturn-smoke-2026-09-04.json", archive);
    touch(f, "{\"run_passed\":false,\"scenarios_total\":1}", now - 500);
    HU_ASSERT_EQ((int)hu_doctor_eval_newest_verdict_unix(archive), 0);
    hu_doctor_eval_freshness_ctx_t ctx = {archive, log, now, 3};
    hu_doctor_check_t c = hu_doctor_check_eval_freshness;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "NEVER produced a real verdict");
    /* A completed multiturn FAIL in the real slot IS a verdict. */
    snprintf(f, sizeof(f), "%s/eval-multiturn-2026-09-04.json", archive);
    touch(f, "{\"run_passed\":false,\"scenarios_total\":8,\"scenarios_passed\":3}", now - 400);
    HU_ASSERT_EQ((long long)hu_doctor_eval_newest_verdict_unix(archive), (long long)(now - 400));
    r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    hu_test_rm_rf(d);
}

static void test_verdict_is_real_truth_table(void) {
    const char *deferred = "{\"verdict\":\"DEFERRED\"}";
    const char *skipped = "{\"verdict\":\"SKIPPED\"}";
    const char *pass = "{\"verdict\":\"PASS\"}";
    const char *fail = "{\"verdict\":\"FAIL\"}";
    const char *mt = "{\"run_passed\":true}";
    const char *empty = "{}";
    const char *junk = "not json";
    HU_ASSERT_FALSE(hu_doctor_eval_verdict_is_real(deferred, strlen(deferred)));
    HU_ASSERT_FALSE(hu_doctor_eval_verdict_is_real(skipped, strlen(skipped)));
    HU_ASSERT_TRUE(hu_doctor_eval_verdict_is_real(pass, strlen(pass)));
    HU_ASSERT_TRUE(hu_doctor_eval_verdict_is_real(fail, strlen(fail)));
    HU_ASSERT_TRUE(hu_doctor_eval_verdict_is_real(mt, strlen(mt)));
    HU_ASSERT_FALSE(hu_doctor_eval_verdict_is_real(empty, strlen(empty)));
    HU_ASSERT_FALSE(hu_doctor_eval_verdict_is_real(junk, strlen(junk)));
    HU_ASSERT_FALSE(hu_doctor_eval_verdict_is_real(NULL, 0));
}

static void test_empty_file_is_not_an_artifact(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_evalfresh", d, sizeof(d)));
    char archive[600], log[600];
    snprintf(archive, sizeof(archive), "%s/archive", d);
    snprintf(log, sizeof(log), "%s/nightly-eval.log", d);
    touch(log, "", 999999);
    HU_ASSERT_EQ((int)hu_doctor_eval_newest_artifact_unix(archive, log), 0);
    hu_doctor_eval_freshness_ctx_t ctx = {archive, log, 1000000, 3};
    hu_doctor_check_t c = hu_doctor_check_eval_freshness;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    hu_test_rm_rf(d);
}

void run_doctor_eval_freshness_tests(void) {
    HU_TEST_SUITE("doctor_eval_freshness");
    HU_RUN_TEST(test_no_artifacts_fails);
    HU_RUN_TEST(test_stale_archive_fails_fresh_passes);
    HU_RUN_TEST(test_empty_file_is_not_an_artifact);
    HU_RUN_TEST(test_deferred_and_smoke_artifacts_are_not_verdicts);
    HU_RUN_TEST(test_verdict_is_real_truth_table);
}
