/* tests/test_doctor_log_hygiene.c — an unbounded launchd log is a FAIL. */
#include "human/doctor/check_ops.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include <stdio.h>
#include <string.h>

static void write_bytes(const char *path, size_t n) {
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (size_t i = 0; i < n; i++)
        fputc('x', f);
    fclose(f);
}

static void test_missing_is_na_small_passes_big_fails(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_loghyg", d, sizeof(d)));
    char p[600];
    snprintf(p, sizeof(p), "%s/service-loop-error.log", d);
    hu_doctor_log_hygiene_ctx_t ctx = {p, 1024};
    hu_doctor_check_t c = hu_doctor_check_log_hygiene;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_NA);
    int64_t none = 7;
    HU_ASSERT_FALSE(hu_doctor_log_size(p, &none));
    write_bytes(p, 512);
    int64_t bytes = 0;
    HU_ASSERT_TRUE(hu_doctor_log_size(p, &bytes));
    HU_ASSERT_EQ((int)bytes, 512);
    r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    HU_ASSERT_STR_CONTAINS(r.detail_json, "\"bytes\":512");
    write_bytes(p, 4096);
    r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "rotate-logs.sh");
    hu_test_rm_rf(d);
}

void run_doctor_log_hygiene_tests(void) {
    HU_TEST_SUITE("doctor_log_hygiene");
    HU_RUN_TEST(test_missing_is_na_small_passes_big_fails);
}
