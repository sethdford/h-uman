/*
 * Test sign-and-notarize.sh script (US-C1.3)
 * Tests: existence, executable, entitlements validation, env-var handling
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_framework.h"

#define SCRIPT_PATH       "scripts/release/sign-and-notarize.sh"
#define ENTITLEMENTS_PATH "scripts/release/entitlements.plist"

/* Helper: check if we're on macOS */
static int is_macos(void) {
    struct utsname buf;
    if (uname(&buf) != 0) {
        return 0;
    }
    return strcmp(buf.sysname, "Darwin") == 0;
}

/* Helper: run a shell command and capture output */
static int run_cmd(const char *cmd, char *output, size_t output_size) {
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        return -1;
    }
    if (output && output_size > 0) {
        size_t n = fread(output, 1, output_size - 1, pipe);
        output[n] = '\0';
    }
    int status = pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Test: script exists and is executable */
static void test_script_exists_and_executable(void) {
    HU_SKIP_IF(!is_macos(), "macOS only");
    HU_ASSERT_TRUE(access(SCRIPT_PATH, X_OK) == 0);
}

/* Test: entitlements.plist exists */
static void test_entitlements_exists(void) {
    HU_SKIP_IF(!is_macos(), "macOS only");
    HU_ASSERT_TRUE(access(ENTITLEMENTS_PATH, F_OK) == 0);
}

/* Test: entitlements.plist is valid XML (plist) */
static void test_entitlements_valid_xml(void) {
    HU_SKIP_IF(!is_macos(), "macOS only");
    HU_SKIP_IF(access(ENTITLEMENTS_PATH, F_OK) != 0, "entitlements.plist not found");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "plutil -lint %s > /dev/null 2>&1", ENTITLEMENTS_PATH);
    int ret = system(cmd);
    HU_ASSERT_TRUE(ret == 0);
}

/* Test: --dry-run flag is accepted and exits 0 */
static void test_dry_run_flag_accepted(void) {
    HU_SKIP_IF(!is_macos(), "macOS only");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "bash %s --pkg /nonexistent.pkg --dry-run 2>&1", SCRIPT_PATH);
    char output[2048] = {0};
    int ret = run_cmd(cmd, output, sizeof(output));

    /* Should not crash; may error on pkg not found or exit 0 if creds are set */
    HU_ASSERT_TRUE(ret == 0 || ret == 1);
}

/* Test: missing APPLE_DEV_ID causes exit 0 (graceful skip) */
static void test_missing_apple_dev_id_exits_zero(void) {
    HU_SKIP_IF(!is_macos(), "macOS only");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "env -u APPLE_DEV_ID bash %s --pkg /tmp/test.pkg --dry-run 2>&1",
             SCRIPT_PATH);

    char output[2048] = {0};
    int ret = run_cmd(cmd, output, sizeof(output));

    HU_ASSERT_EQ(ret, 0);
    HU_ASSERT_TRUE(strstr(output, "APPLE_DEV_ID") != NULL);
}

/* Test: missing NOTARY_PROFILE causes exit 0 (graceful skip) */
static void test_missing_notary_profile_exits_zero(void) {
    HU_SKIP_IF(!is_macos(), "macOS only");

    char cmd[768];
    snprintf(cmd, sizeof(cmd),
             "env -u NOTARY_PROFILE APPLE_DEV_ID='test' APPLE_DEV_ID_INSTALLER='test' "
             "bash %s --pkg /tmp/test.pkg --dry-run 2>&1",
             SCRIPT_PATH);

    char output[2048] = {0};
    int ret = run_cmd(cmd, output, sizeof(output));

    HU_ASSERT_EQ(ret, 0);
    HU_ASSERT_TRUE(strstr(output, "NOTARY_PROFILE") != NULL);
}

/* Test: non-existent .pkg returns error */
static void test_missing_pkg_returns_error(void) {
    HU_SKIP_IF(!is_macos(), "macOS only");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "bash %s --pkg /nonexistent/test.pkg 2>&1", SCRIPT_PATH);

    char output[2048] = {0};
    int ret = run_cmd(cmd, output, sizeof(output));

    HU_ASSERT_TRUE(ret != 0);
    HU_ASSERT_TRUE(strstr(output, "not found") != NULL);
}

/* Test: non-macOS returns exit code 79 */
static void test_non_macos_returns_79(void) {
    HU_SKIP_IF(is_macos(), "non-macOS only");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "bash %s --dry-run 2>&1", SCRIPT_PATH);

    char output[2048] = {0};
    int ret = run_cmd(cmd, output, sizeof(output));

    HU_ASSERT_EQ(ret, 79);
    HU_ASSERT_TRUE(strstr(output, "macOS") != NULL);
}

/* Test: invalid argument shows Usage */
static void test_invalid_argument_shows_usage(void) {
    HU_SKIP_IF(!is_macos(), "macOS only");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "bash %s --invalid-flag 2>&1", SCRIPT_PATH);

    char output[2048] = {0};
    int ret = run_cmd(cmd, output, sizeof(output));

    HU_ASSERT_TRUE(ret != 0);
    HU_ASSERT_TRUE(strstr(output, "Usage") != NULL);
}

void run_sign_notarize_tests(void) {
    HU_TEST_SUITE("sign_notarize");
    HU_RUN_TEST(test_script_exists_and_executable);
    HU_RUN_TEST(test_entitlements_exists);
    HU_RUN_TEST(test_entitlements_valid_xml);
    HU_RUN_TEST(test_dry_run_flag_accepted);
    HU_RUN_TEST(test_missing_apple_dev_id_exits_zero);
    HU_RUN_TEST(test_missing_notary_profile_exits_zero);
    HU_RUN_TEST(test_missing_pkg_returns_error);
    HU_RUN_TEST(test_non_macos_returns_79);
    HU_RUN_TEST(test_invalid_argument_shows_usage);
}
