/**
 * Test the macOS app bundle structure (US-C1.1).
 *
 * Verifies that the bundle directory tree exists, Info.plist is valid XML,
 * and that required keys are present in the plist.
 */

#include "test_framework.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * Helper: check if a file exists and is readable.
 */
static bool file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

/**
 * Helper: check if a file is executable.
 */
static bool file_is_executable(const char *path) {
    struct stat sb;
    return stat(path, &sb) == 0 && (sb.st_mode & S_IXUSR);
}

/**
 * Helper: run plutil to validate Info.plist syntax.
 * Returns true if plutil -lint succeeds.
 *
 * Note: plutil is a macOS tool. On non-macOS, this test gracefully
 * skips bundle validation.
 */
static bool validate_plist_with_plutil(const char *plist_path) {
#ifdef __APPLE__
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "plutil -lint '%s' >/dev/null 2>&1", plist_path);
    int ret = system(cmd);
    return ret == 0;
#else
    (void)plist_path;
    return true; /* skip on non-macOS */
#endif
}

/**
 * Test that the bundle directory structure exists.
 */
static void test_bundle_directory_structure_exists(void) {
#ifdef __APPLE__
    /* Bundle is at Human.app/Contents when test runs from build directory */
    HU_ASSERT(file_exists("Human.app/Contents"));
    HU_ASSERT(file_exists("Human.app/Contents/MacOS"));
    HU_ASSERT(file_exists("Human.app/Contents/Resources"));
#endif
}

/**
 * Test that Info.plist exists and is valid XML.
 */
static void test_info_plist_exists_and_valid(void) {
#ifdef __APPLE__
    HU_ASSERT(file_exists("Human.app/Contents/Info.plist"));
    HU_ASSERT(validate_plist_with_plutil("Human.app/Contents/Info.plist"));
#endif
}

/**
 * Test that Info.plist contains required keys.
 */
static void test_info_plist_has_required_keys(void) {
#ifdef __APPLE__
    /* We verify via plutil that critical keys exist.
     * plutil -p outputs all keys; we check if the key line is present.
     */
    FILE *fp;
    char cmd[256];
    char buf[512];
    bool has_bundle_id = false;
    bool has_bundle_name = false;
    bool has_bundle_version = false;
    bool has_bundle_executable = false;
    bool has_bundle_package_type = false;

    snprintf(cmd, sizeof(cmd), "plutil -p 'Human.app/Contents/Info.plist' 2>/dev/null");
    fp = popen(cmd, "r");
    HU_ASSERT(fp != NULL);

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        if (strstr(buf, "CFBundleIdentifier")) {
            has_bundle_id = true;
        }
        if (strstr(buf, "CFBundleName")) {
            has_bundle_name = true;
        }
        if (strstr(buf, "CFBundleVersion")) {
            has_bundle_version = true;
        }
        if (strstr(buf, "CFBundleExecutable")) {
            has_bundle_executable = true;
        }
        if (strstr(buf, "CFBundlePackageType")) {
            has_bundle_package_type = true;
        }
    }
    pclose(fp);

    HU_ASSERT(has_bundle_id);
    HU_ASSERT(has_bundle_name);
    HU_ASSERT(has_bundle_version);
    HU_ASSERT(has_bundle_executable);
    HU_ASSERT(has_bundle_package_type);
#endif
}

/**
 * Test that the daemon binary exists in MacOS/ and is executable.
 */
static void test_daemon_binary_present_and_executable(void) {
#ifdef __APPLE__
    HU_ASSERT(file_exists("Human.app/Contents/MacOS/human"));
    HU_ASSERT(file_is_executable("Human.app/Contents/MacOS/human"));
#endif
}

/**
 * Run all app bundle structure tests.
 */
void run_app_bundle_structure_tests(void) {
    HU_TEST_SUITE("app_bundle_structure");

    /* Directory structure */
    HU_RUN_TEST(test_bundle_directory_structure_exists);

    /* Info.plist validity */
    HU_RUN_TEST(test_info_plist_exists_and_valid);
    HU_RUN_TEST(test_info_plist_has_required_keys);

    /* Daemon binary */
    HU_RUN_TEST(test_daemon_binary_present_and_executable);
}
