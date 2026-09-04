/* tests/test_doctor_serving_stability.c — 25 crash reports in a morning must
 * not read as "inference ok". */
#include "human/doctor/check_ops.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

static void touch_at(const char *dir, const char *name, int64_t mtime) {
    char p[800];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs("{}", f);
        fclose(f);
    }
    struct utimbuf t = {.actime = (time_t)mtime, .modtime = (time_t)mtime};
    utime(p, &t);
}

static const char *k_launchctl_loop = "ai.human.mlx-server = {\n\tstate = running\n\truns = 34\n"
                                      "\tlast exit code = 11\n\tpid = 68889\n}\n";
static const char *k_launchctl_calm = "\tstate = running\n\truns = 2\n\tlast exit code = 0\n";

static void test_parsers(void) {
    HU_ASSERT_EQ((int)hu_doctor_launchctl_runs(k_launchctl_loop), 34);
    HU_ASSERT_EQ((int)hu_doctor_launchctl_last_exit(k_launchctl_loop), 11);
    HU_ASSERT_EQ((int)hu_doctor_launchctl_runs(NULL), -1);
    HU_ASSERT_EQ((int)hu_doctor_launchctl_runs("no such field"), -1);
}

static void test_crash_loop_fails(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_serving", d, sizeof(d)));
    int64_t now = 2000000;
    touch_at(d, "Python-2026-09-02-004018.ips", now - 3600);
    touch_at(d, "Python-2026-09-02-013216.ips", now - 1800);
    touch_at(d, "Python-2026-09-02-014808.ips", now - 600);
    touch_at(d, "Safari-2026-09-02-014808.ips", now - 600);        /* other process: ignored */
    touch_at(d, "Python-2026-08-01-000000.ips", now - 40 * 86400); /* outside window */
    hu_doctor_serving_stability_ctx_t ctx = {d, "Python-", k_launchctl_loop, now, 24, 2};
    hu_doctor_check_t c = hu_doctor_check_serving_stability;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "crash-looping: 3");
    HU_ASSERT_STR_CONTAINS(r.detail_json, "\"crash_reports_24h\":3");
    HU_ASSERT_STR_CONTAINS(r.detail_json, "\"launchd_runs\":34");
    HU_ASSERT_STR_CONTAINS(r.detail_json, "\"last_exit_code\":11");
    hu_test_rm_rf(d);
}

static void test_abnormal_last_exit_fails_even_without_reports(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_serving", d, sizeof(d)));
    hu_doctor_serving_stability_ctx_t ctx = {d, "Python-", k_launchctl_loop, 2000000, 24, 2};
    hu_doctor_check_t c = hu_doctor_check_serving_stability;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "abnormal");
    hu_test_rm_rf(d);
}

static void test_calm_passes_and_nothing_available_is_na(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_serving", d, sizeof(d)));
    touch_at(d, "Python-2026-09-01-000000.ips", 2000000 - 7200);
    hu_doctor_serving_stability_ctx_t ctx = {d, "Python-", k_launchctl_calm, 2000000, 24, 2};
    hu_doctor_check_t c = hu_doctor_check_serving_stability;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    hu_test_rm_rf(d);
    char missing[600];
    snprintf(missing, sizeof(missing), "%s/does-not-exist", d);
    hu_doctor_serving_stability_ctx_t na = {missing, "Python-", NULL, 2000000, 24, 2};
    r = c.run(&c, &na);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_NA);
}

void run_doctor_serving_stability_tests(void) {
    HU_TEST_SUITE("doctor_serving_stability");
    HU_RUN_TEST(test_parsers);
    HU_RUN_TEST(test_crash_loop_fails);
    HU_RUN_TEST(test_abnormal_last_exit_fails_even_without_reports);
    HU_RUN_TEST(test_calm_passes_and_nothing_available_is_na);
}
