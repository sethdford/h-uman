#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor.h"
#include "human/doctor/check.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Forward declaration of the check */
extern hu_doctor_check_t hu_doctor_check_chatdb;

/* Test: chatdb_readable returns missing when file doesn't exist */
static void test_chatdb_missing_returns_fail_with_missing_reason(void) {
    /* Create a temporary directory to serve as HOME with no chat.db */
    char temp_home[256];
    snprintf(temp_home, sizeof(temp_home), "/tmp/hu_test_chatdb_XXXXXX");

    if (!mkdtemp(temp_home)) {
        HU_ASSERT(0); /* mkdtemp failed */
    }

    /* Set HOME to the temporary directory */
    const char *old_home = getenv("HOME");
    if (setenv("HOME", temp_home, 1) != 0) {
        HU_ASSERT(0); /* setenv failed */
    }

    /* Create the Library/Messages directory structure so we're not failing due to
     * missing parent dirs, only missing chat.db */
    char lib_path[512];
    snprintf(lib_path, sizeof(lib_path), "%s/Library/Messages", temp_home);
    mkdir(lib_path, 0755); /* Create parent dirs if needed */

    /* Ensure chat.db doesn't exist */
    char chatdb_path[512];
    snprintf(chatdb_path, sizeof(chatdb_path), "%s/chat.db", lib_path);
    unlink(chatdb_path); /* Remove if it exists */

    /* Run the check */
    hu_doctor_check_result_t result = hu_doctor_check_chatdb.run(&hu_doctor_check_chatdb, NULL);

    /* Verify: must return FAIL with reason "missing" */
    HU_ASSERT_EQ((int)result.verdict, (int)HU_DOCTOR_FAIL);
    HU_ASSERT(result.reason != NULL);
    HU_ASSERT(strstr(result.reason, "missing") != NULL);

    /* Cleanup: restore HOME and remove temp directory */
    if (old_home) {
        setenv("HOME", old_home, 1);
    } else {
        unsetenv("HOME");
    }
    rmdir(chatdb_path);
    rmdir(lib_path);
    rmdir(temp_home);
}

/* Test: chatdb_readable returns permission_denied with System Settings link when
 * file exists but is not readable */
static void test_chatdb_permission_denied_returns_fail_with_fda_link(void) {
    /* Create a temporary directory to serve as HOME */
    char temp_home[256];
    snprintf(temp_home, sizeof(temp_home), "/tmp/hu_test_chatdb_perm_XXXXXX");

    if (!mkdtemp(temp_home)) {
        HU_ASSERT(0); /* mkdtemp failed */
    }

    /* Set HOME to the temporary directory */
    const char *old_home = getenv("HOME");
    if (setenv("HOME", temp_home, 1) != 0) {
        HU_ASSERT(0); /* setenv failed */
    }

    /* Create the Library/Messages directory */
    char lib_path[512];
    snprintf(lib_path, sizeof(lib_path), "%s/Library/Messages", temp_home);
    mkdir(lib_path, 0755);

    /* Create a fake chat.db file with mode 000 (no read permission) */
    char chatdb_path[512];
    snprintf(chatdb_path, sizeof(chatdb_path), "%s/chat.db", lib_path);
    FILE *f = fopen(chatdb_path, "w");
    if (f) {
        fclose(f);
    }
    /* Change permissions to 000 to simulate permission denied */
    chmod(chatdb_path, 0000);

    /* Run the check */
    hu_doctor_check_result_t result = hu_doctor_check_chatdb.run(&hu_doctor_check_chatdb, NULL);

    /* Verify: must return FAIL with permission denied message containing System
     * Settings guidance */
    HU_ASSERT_EQ((int)result.verdict, (int)HU_DOCTOR_FAIL);
    HU_ASSERT(result.reason != NULL);
    HU_ASSERT(strstr(result.reason, "System Settings") != NULL);
    HU_ASSERT(strstr(result.reason, "Full Disk Access") != NULL);
    HU_ASSERT(strstr(result.reason, "x-apple.systempreferences") != NULL);

    /* Cleanup: restore HOME and permissions, then remove temp directory */
    if (old_home) {
        setenv("HOME", old_home, 1);
    } else {
        unsetenv("HOME");
    }
    chmod(chatdb_path, 0644); /* Restore permissions so we can delete it */
    unlink(chatdb_path);
    rmdir(lib_path);
    rmdir(temp_home);
}

/* Test: on non-macOS platforms, check returns NA */
#ifndef __APPLE__
static void test_chatdb_platform_not_applicable_returns_na(void) {
    hu_doctor_check_result_t result = hu_doctor_check_chatdb.run(&hu_doctor_check_chatdb, NULL);
    HU_ASSERT_EQ((int)result.verdict, (int)HU_DOCTOR_NA);
}
#endif

/* Test runner */
void run_doctor_chatdb_tests(void) {
    HU_TEST_SUITE("doctor_chatdb");

#ifdef __APPLE__
    HU_RUN_TEST(test_chatdb_missing_returns_fail_with_missing_reason);
    HU_RUN_TEST(test_chatdb_permission_denied_returns_fail_with_fda_link);
#else
    HU_RUN_TEST(test_chatdb_platform_not_applicable_returns_na);
#endif
}
