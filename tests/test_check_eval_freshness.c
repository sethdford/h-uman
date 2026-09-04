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
    /* a fresh nightly-eval.log alone is enough */
    touch(log, "[2026-09-02T04:05:00] === nightly_eval done ===\n", now - 3600);
    r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    hu_test_rm_rf(d);
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
}
