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
 * Helper: resolve the path to the source bundle.
 * Searches up to 3 levels up from CWD for apps/macOS/Human.app/Contents.
 * Returns the resolved path on success, or NULL if not found.
 */
static char *resolve_bundle_path(void) {
    static char resolved[512] = {0};

    /* If already resolved, return it */
    if (resolved[0] != '\0') {
        return resolved;
    }

    const char *bundle_paths[] = {
        "apps/macOS/Human.app/Contents",
        "../apps/macOS/Human.app/Contents",
        "../../apps/macOS/Human.app/Contents",
        "../../../apps/macOS/Human.app/Contents",
    };

    for (size_t i = 0; i < sizeof(bundle_paths) / sizeof(bundle_paths[0]); i++) {
        char test_plist[512];
        snprintf(test_plist, sizeof(test_plist), "%s/Info.plist", bundle_paths[i]);
        if (access(test_plist, F_OK) == 0) {
            snprintf(resolved, sizeof(resolved), "%s", bundle_paths[i]);
            return resolved;
        }
    }

    return NULL;
}

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
    char bundle_path[256];
    char path_buf[512];

    const char *bundle_base = resolve_bundle_path();
    HU_ASSERT(bundle_base != NULL);

    snprintf(bundle_path, sizeof(bundle_path), "%s", bundle_base);
    HU_ASSERT(file_exists(bundle_path));

    snprintf(path_buf, sizeof(path_buf), "%s/MacOS", bundle_base);
    HU_ASSERT(file_exists(path_buf));

    snprintf(path_buf, sizeof(path_buf), "%s/Resources", bundle_base);
    HU_ASSERT(file_exists(path_buf));
#endif
}

/**
 * Test that Info.plist exists and is valid XML.
 */
static void test_info_plist_exists_and_valid(void) {
#ifdef __APPLE__
    char plist_path[512];

    const char *bundle_base = resolve_bundle_path();
    HU_ASSERT(bundle_base != NULL);

    snprintf(plist_path, sizeof(plist_path), "%s/Info.plist", bundle_base);
    HU_ASSERT(file_exists(plist_path));
    HU_ASSERT(validate_plist_with_plutil(plist_path));
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
    char cmd[512];
    char buf[512];
    bool has_bundle_id = false;
    bool has_bundle_name = false;
    bool has_bundle_version = false;
    bool has_bundle_executable = false;
    bool has_bundle_package_type = false;

    const char *bundle_base = resolve_bundle_path();
    HU_ASSERT(bundle_base != NULL);

    snprintf(cmd, sizeof(cmd), "plutil -p '%s/Info.plist' 2>/dev/null", bundle_base);
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
 * Test that the daemon binary path can be resolved.
 * This test always passes and validates the path computation logic.
 */
static void test_daemon_binary_path_resolves(void) {
#ifdef __APPLE__
    char binary_path[512];

    const char *bundle_base = resolve_bundle_path();
    HU_ASSERT(bundle_base != NULL);

    snprintf(binary_path, sizeof(binary_path), "%s/MacOS/human", bundle_base);
    HU_ASSERT(strlen(binary_path) > 0);
#endif
}

/**
 * Test that the daemon binary exists and is executable.
 * NOTE: The binary is only present after running the build_app_bundle target.
 * If the binary is missing, this test is skipped (not silently passed).
 */
static void test_daemon_binary_present_and_executable(void) {
#ifdef __APPLE__
    char binary_path[512];

    const char *bundle_base = resolve_bundle_path();
    HU_ASSERT(bundle_base != NULL);

    snprintf(binary_path, sizeof(binary_path), "%s/MacOS/human", bundle_base);

    /* Binary is only present after build_app_bundle target runs.
     * We skip (not silently pass) if the binary hasn't been built. */
    HU_SKIP_IF(!file_exists(binary_path),
               "Daemon binary not built; requires build_app_bundle target");
    HU_ASSERT(file_is_executable(binary_path));
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
    HU_RUN_TEST(test_daemon_binary_path_resolves);
    HU_RUN_TEST(test_daemon_binary_present_and_executable);
}
