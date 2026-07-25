/* tests/test_state_file.c
 *
 * hu_state_file_* — atomic state-file write helper (src/core/state_file.c,
 * carved from daemon state persistence). Pins:
 *   - default_path is a hard NULL under HU_IS_TEST (tests must never touch
 *     the real ~/.human) and NULL-safe on bad args
 *   - begin/commit is atomic: content lands at the final path only on
 *     write_ok, the .tmp staging file never survives either outcome
 *   - failed commit (write_ok=false) leaves NO file at the final path
 */

#include "human/core/state_file.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool file_exists(const char *path) {
    struct stat sb;
    return stat(path, &sb) == 0;
}

static void test_state_file_default_path_null_in_test_mode(void) {
    /* HU_IS_TEST contract: never resolve into the real home dir. */
    char buf[512];
    HU_ASSERT_TRUE(hu_state_file_default_path("anything.json", buf, sizeof(buf)) == NULL);
    HU_ASSERT_TRUE(hu_state_file_default_path(NULL, buf, sizeof(buf)) == NULL);
    HU_ASSERT_TRUE(hu_state_file_default_path("x.json", NULL, 0) == NULL);
}

static void test_state_file_commit_renames_tmp_to_final_path(void) {
    char path[256], tmp[300];
    snprintf(path, sizeof(path), "/tmp/hu_state_file_test_%d.json", (int)getpid());
    remove(path);

    FILE *f = hu_state_file_write_begin(path, tmp, sizeof(tmp));
    HU_ASSERT_NOT_NULL(f);
    /* staging file exists at <path>.tmp, final path not yet written */
    HU_ASSERT_TRUE(strstr(tmp, ".tmp") != NULL);
    HU_ASSERT_TRUE(file_exists(tmp));
    HU_ASSERT_TRUE(!file_exists(path));

    fputs("{\"ok\":true}", f);
    HU_ASSERT_EQ((int)hu_state_file_write_commit(f, true, tmp, path), (int)HU_OK);

    /* committed: final path has the content, staging file is gone */
    HU_ASSERT_TRUE(file_exists(path));
    HU_ASSERT_TRUE(!file_exists(tmp));
    FILE *r = fopen(path, "r");
    HU_ASSERT_NOT_NULL(r);
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, r);
    fclose(r);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, "{\"ok\":true}");
    remove(path);
}

static void test_state_file_failed_commit_removes_tmp_and_leaves_no_file(void) {
    char path[256], tmp[300];
    snprintf(path, sizeof(path), "/tmp/hu_state_file_fail_%d.json", (int)getpid());
    remove(path);

    FILE *f = hu_state_file_write_begin(path, tmp, sizeof(tmp));
    HU_ASSERT_NOT_NULL(f);
    fputs("partial", f);
    /* write_ok=false = the caller's writes failed mid-way: commit must
     * refuse, clean up the staging file, and leave the final path absent */
    HU_ASSERT_EQ((int)hu_state_file_write_commit(f, false, tmp, path), (int)HU_ERR_IO);
    HU_ASSERT_TRUE(!file_exists(tmp));
    HU_ASSERT_TRUE(!file_exists(path));
}

static void test_state_file_begin_rejects_bad_args(void) {
    char tmp[300];
    HU_ASSERT_TRUE(hu_state_file_write_begin(NULL, tmp, sizeof(tmp)) == NULL);
    HU_ASSERT_TRUE(hu_state_file_write_begin("", tmp, sizeof(tmp)) == NULL);
    HU_ASSERT_TRUE(hu_state_file_write_begin("/tmp/x.json", NULL, 0) == NULL);
    /* tmp buffer too small for "<path>.tmp" must fail, not truncate */
    char tiny[4];
    HU_ASSERT_TRUE(hu_state_file_write_begin("/tmp/hu_state_tiny.json", tiny, sizeof(tiny)) ==
                   NULL);
    HU_ASSERT_EQ((int)hu_state_file_write_commit(NULL, true, "t", "p"),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

void run_state_file_tests(void) {
    HU_TEST_SUITE("core state_file");
    HU_RUN_TEST(test_state_file_default_path_null_in_test_mode);
    HU_RUN_TEST(test_state_file_commit_renames_tmp_to_final_path);
    HU_RUN_TEST(test_state_file_failed_commit_removes_tmp_and_leaves_no_file);
    HU_RUN_TEST(test_state_file_begin_rejects_bad_args);
}
