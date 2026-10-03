/* tests/test_check_adapter_disk.c — nightly adapter candidates must not be
 * allowed to fill the disk silently; see include/human/doctor/check_ops.h. */
#include "human/doctor/check_ops.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void write_file(const char *path, size_t n) {
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (size_t i = 0; i < n; i++)
        fputc('x', f);
    fclose(f);
}

/* Pure predicate: non-vacuous truth table over the three thresholds. */
static void test_should_fail_predicate(void) {
    /* plenty of free space, tiny candidate total -> PASS */
    HU_ASSERT_FALSE(hu_doctor_adapter_disk_should_fail(80, 2, 50, 10, 20));
    /* free space below warn (50) but above error (10) -> FAIL */
    HU_ASSERT_TRUE(hu_doctor_adapter_disk_should_fail(30, 2, 50, 10, 20));
    /* free space below error (10) -> FAIL */
    HU_ASSERT_TRUE(hu_doctor_adapter_disk_should_fail(5, 2, 50, 10, 20));
    /* free space fine, but candidates over the 20 GB warn line -> FAIL */
    HU_ASSERT_TRUE(hu_doctor_adapter_disk_should_fail(80, 25, 50, 10, 20));
    /* exactly at the boundaries: free==warn (not below), candidates==warn (not over) -> PASS */
    HU_ASSERT_FALSE(hu_doctor_adapter_disk_should_fail(50, 20, 50, 10, 20));
}

/* dir_size_bytes: real filesystem, so this is the one sub-test that touches
 * disk — a temp dir, never a symlink, never ~/.human. */
static void test_dir_size_bytes_recursive_and_symlink_safe(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_adisk", d, sizeof(d)));
    char sub[600], f1[700], f2[700], outside[700], link[700];
    snprintf(sub, sizeof(sub), "%s/nested", d);
    mkdir(sub, 0700);
    snprintf(f1, sizeof(f1), "%s/a.bin", d);
    snprintf(f2, sizeof(f2), "%s/b.bin", sub);
    write_file(f1, 1000);
    write_file(f2, 2000);
    /* a symlink inside the tree pointing OUTSIDE must never be followed */
    snprintf(outside, sizeof(outside), "%s/../hu_adisk_outside", d);
    write_file(outside, 999999);
    snprintf(link, sizeof(link), "%s/escape", d);
    symlink(outside, link);

    HU_ASSERT_EQ((int)hu_doctor_dir_size_bytes(d), 3000);
    HU_ASSERT_EQ((int)hu_doctor_dir_size_bytes("/path/does/not/exist"), 0);

    unlink(outside);
    hu_test_rm_rf(d);
}

/* run(): the PASS/FAIL contract with injected (hermetic) measurements, and
 * the fix (HU_ADAPTER_PRUNE=live) named in the FAIL reason. */
static void test_run_pass_and_fail_and_names_the_fix(void) {
    hu_doctor_check_t c = hu_doctor_check_adapter_disk;

    hu_doctor_adapter_disk_ctx_t ok = {
        .adapters_dir = "/tmp/does-not-need-to-exist-for-injected-values",
        .free_bytes = (int64_t)80 * 1024 * 1024 * 1024,
        .candidate_bytes = (int64_t)2 * 1024 * 1024 * 1024,
        .warn_free_gb = 50,
        .error_free_gb = 10,
        .candidate_warn_gb = 20,
    };
    hu_doctor_check_result_t r = c.run(&c, &ok);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);

    hu_doctor_adapter_disk_ctx_t low = ok;
    low.free_bytes = (int64_t)5 * 1024 * 1024 * 1024; /* below error_free_gb */
    r = c.run(&c, &low);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "HU_ADAPTER_PRUNE=live");
    HU_ASSERT_STR_CONTAINS(r.reason, "CRITICAL");

    hu_doctor_adapter_disk_ctx_t too_many = ok;
    too_many.candidate_bytes = (int64_t)25 * 1024 * 1024 * 1024; /* over candidate_warn_gb */
    r = c.run(&c, &too_many);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "HU_ADAPTER_PRUNE=live");
}

/* run() with ctx == NULL must not crash (production default path resolution
 * via hu_paths_state); only assert it returns SOME verdict. */
static void test_run_null_ctx_resolves_defaults_without_crashing(void) {
    hu_doctor_check_t c = hu_doctor_check_adapter_disk;
    hu_doctor_check_result_t r = c.run(&c, NULL);
    HU_ASSERT_TRUE(r.verdict == HU_DOCTOR_PASS || r.verdict == HU_DOCTOR_FAIL ||
                   r.verdict == HU_DOCTOR_NA);
}

void run_doctor_adapter_disk_tests(void) {
    HU_TEST_SUITE("doctor_adapter_disk");
    HU_RUN_TEST(test_should_fail_predicate);
    HU_RUN_TEST(test_dir_size_bytes_recursive_and_symlink_safe);
    HU_RUN_TEST(test_run_pass_and_fail_and_names_the_fix);
    HU_RUN_TEST(test_run_null_ctx_resolves_defaults_without_crashing);
}
