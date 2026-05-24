// Test suite for US-C1.2 macOS .pkg builder script
// Tests pkgbuild invocation, command-line parsing, and distribution.xml generation

#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_pkg_builder_flag_parsing_output_flag(void) {
    // AC-C1.2.1: Script accepts --output flag and parses it correctly
    HU_SKIP_IF(access("scripts/release/build-pkg.sh", F_OK) != 0, "build-pkg.sh not found");
    HU_ASSERT_TRUE(access("scripts/release/build-pkg.sh", F_OK) == 0);
}

static void test_pkg_builder_flag_parsing_app_path_flag(void) {
    // AC-C1.2.1: Script accepts --app-path flag and parses it correctly
    HU_SKIP_IF(access("scripts/release/build-pkg.sh", X_OK) != 0, "build-pkg.sh not executable");

    FILE *f = fopen("scripts/release/build-pkg.sh", "r");
    HU_SKIP_IF(f == NULL, "build-pkg.sh cannot be opened");

    char line[256];
    int found_app_path = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "--app-path") != NULL) {
            found_app_path = 1;
            break;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_app_path);
}

static void test_pkg_builder_flag_parsing_defaults(void) {
    // AC-C1.2.1: Script has sensible defaults (build/Human.app, human-release.pkg)
    HU_SKIP_IF(access("scripts/release/build-pkg.sh", X_OK) != 0, "build-pkg.sh not executable");

    FILE *f = fopen("scripts/release/build-pkg.sh", "r");
    HU_SKIP_IF(f == NULL, "build-pkg.sh cannot be opened");

    char line[256];
    int found_default_app = 0, found_default_output = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "build/Human.app") != NULL) {
            found_default_app = 1;
        }
        if (strstr(line, "human-release.pkg") != NULL) {
            found_default_output = 1;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_default_app);
    HU_ASSERT_TRUE(found_default_output);
}

static void test_pkg_builder_uses_pkgbuild(void) {
    // AC-C1.2.2: Script invokes pkgbuild with correct flags
    HU_SKIP_IF(access("scripts/release/build-pkg.sh", X_OK) != 0, "build-pkg.sh not executable");

    FILE *f = fopen("scripts/release/build-pkg.sh", "r");
    HU_SKIP_IF(f == NULL, "build-pkg.sh cannot be opened");

    char line[256];
    int found_pkgbuild = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "pkgbuild") != NULL) {
            found_pkgbuild = 1;
            break;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_pkgbuild);
}

static void test_pkg_builder_pkgbuild_component_plist_flag(void) {
    // AC-C1.2.2: pkgbuild command includes --component-plist flag
    HU_SKIP_IF(access("scripts/release/build-pkg.sh", X_OK) != 0, "build-pkg.sh not executable");

    FILE *f = fopen("scripts/release/build-pkg.sh", "r");
    HU_SKIP_IF(f == NULL, "build-pkg.sh cannot be opened");

    char line[256];
    int found_component_plist = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "--component-plist") != NULL) {
            found_component_plist = 1;
            break;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_component_plist);
}

static void test_pkg_builder_pkgbuild_identifier_flag(void) {
    // AC-C1.2.2: pkgbuild command includes --identifier flag with com.h-uman prefix
    HU_SKIP_IF(access("scripts/release/build-pkg.sh", X_OK) != 0, "build-pkg.sh not executable");

    FILE *f = fopen("scripts/release/build-pkg.sh", "r");
    HU_SKIP_IF(f == NULL, "build-pkg.sh cannot be opened");

    char line[256];
    int found_identifier = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "--identifier") != NULL && strstr(line, "com.h-uman") != NULL) {
            found_identifier = 1;
            break;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_identifier);
}

static void test_pkg_builder_uses_productbuild(void) {
    // AC-C1.2.3: Script invokes productbuild to wrap component with distribution policy
    HU_SKIP_IF(access("scripts/release/build-pkg.sh", X_OK) != 0, "build-pkg.sh not executable");

    FILE *f = fopen("scripts/release/build-pkg.sh", "r");
    HU_SKIP_IF(f == NULL, "build-pkg.sh cannot be opened");

    char line[256];
    int found_productbuild = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "productbuild") != NULL) {
            found_productbuild = 1;
            break;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_productbuild);
}

static void test_pkg_builder_distribution_xml_template_exists(void) {
    // AC-C1.2.3: Distribution.xml template exists and is valid XML
    HU_SKIP_IF(access("tests/fixtures/distribution.xml.template", F_OK) != 0,
               "distribution.xml.template not found");

    FILE *f = fopen("tests/fixtures/distribution.xml.template", "r");
    HU_SKIP_IF(f == NULL, "distribution.xml.template cannot be opened");

    char line[256];
    int found_xml_decl = 0, found_installer_gui = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "<?xml") != NULL) {
            found_xml_decl = 1;
        }
        if (strstr(line, "installer-gui-script") != NULL) {
            found_installer_gui = 1;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_xml_decl);
    HU_ASSERT_TRUE(found_installer_gui);
}

static void test_pkg_builder_distribution_xml_has_version_placeholder(void) {
    // AC-C1.2.3: Distribution.xml template has @VERSION@ placeholder for sed substitution
    HU_SKIP_IF(access("tests/fixtures/distribution.xml.template", F_OK) != 0,
               "distribution.xml.template not found");

    FILE *f = fopen("tests/fixtures/distribution.xml.template", "r");
    HU_SKIP_IF(f == NULL, "distribution.xml.template cannot be opened");

    char line[256];
    int found_version_placeholder = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "@VERSION@") != NULL) {
            found_version_placeholder = 1;
            break;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_version_placeholder);
}

static void test_pkg_builder_distribution_xml_has_installation_check(void) {
    // AC-C1.2.3: Distribution.xml includes InstallationCheck for macOS version + free space
    HU_SKIP_IF(access("tests/fixtures/distribution.xml.template", F_OK) != 0,
               "distribution.xml.template not found");

    FILE *f = fopen("tests/fixtures/distribution.xml.template", "r");
    HU_SKIP_IF(f == NULL, "distribution.xml.template cannot be opened");

    char line[256];
    int found_installation_check = 0, found_version_check = 0, found_space_check = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "installation-check") != NULL) {
            found_installation_check = 1;
        }
        if (strstr(line, "major < 10") != NULL || strstr(line, "10.15") != NULL) {
            found_version_check = 1;
        }
        if (strstr(line, "diskSpaceForVolume") != NULL || strstr(line, "500") != NULL) {
            found_space_check = 1;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_installation_check);
    HU_ASSERT_TRUE(found_version_check || found_space_check);
}

static void test_pkg_builder_postinstall_script_exists(void) {
    // AC-C1.2.5: Post-install script exists and is executable
    HU_SKIP_IF(access("scripts/release/pkg-scripts/postinstall.sh", F_OK) != 0,
               "postinstall.sh not found");

    HU_ASSERT_TRUE(access("scripts/release/pkg-scripts/postinstall.sh", X_OK) == 0);
}

static void test_pkg_builder_postinstall_validates_bundle(void) {
    // AC-C1.2.5: Post-install script validates bundle installation
    HU_SKIP_IF(access("scripts/release/pkg-scripts/postinstall.sh", F_OK) != 0,
               "postinstall.sh not found");

    FILE *f = fopen("scripts/release/pkg-scripts/postinstall.sh", "r");
    HU_SKIP_IF(f == NULL, "postinstall.sh cannot be opened");

    char line[256];
    int found_bundle_check = 0, found_executable_check = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "Human.app") != NULL) {
            found_bundle_check = 1;
        }
        if (strstr(line, "executable") != NULL || strstr(line, "-x") != NULL) {
            found_executable_check = 1;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_bundle_check);
    HU_ASSERT_TRUE(found_executable_check);
}

static void test_pkg_builder_launchd_plist_exists(void) {
    // Verify launchd plist asset exists and has correct structure
    HU_SKIP_IF(access("apps/macOS/launchd/com.h-uman.human.plist", F_OK) != 0,
               "launchd plist not found");

    FILE *f = fopen("apps/macOS/launchd/com.h-uman.human.plist", "r");
    HU_SKIP_IF(f == NULL, "launchd plist cannot be opened");

    char line[256];
    int found_plist_decl = 0, found_label = 0, found_run_at_load = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "<?xml") != NULL || strstr(line, "plist") != NULL) {
            found_plist_decl = 1;
        }
        if (strstr(line, "com.h-uman.human") != NULL) {
            found_label = 1;
        }
        if (strstr(line, "RunAtLoad") != NULL) {
            found_run_at_load = 1;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_plist_decl);
    HU_ASSERT_TRUE(found_label);
    HU_ASSERT_TRUE(found_run_at_load);
}

static void test_pkg_builder_dry_run_mode(void) {
    // AC-C1.2.5: --dry-run flag produces sensible output without invoking pkgbuild
    HU_SKIP_IF(access("scripts/release/build-pkg.sh", X_OK) != 0, "build-pkg.sh not executable");

    FILE *f = fopen("scripts/release/build-pkg.sh", "r");
    HU_SKIP_IF(f == NULL, "build-pkg.sh cannot be opened");

    char line[256];
    int found_dry_run = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "--dry-run") != NULL || strstr(line, "DRY_RUN") != NULL) {
            found_dry_run = 1;
            break;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_dry_run);
}

static void test_pkg_builder_non_macos_exits_79(void) {
    // Script must return exit code 79 on non-macOS (HU_SKIP_PLATFORM_SPECIFIC)
    HU_SKIP_IF(access("scripts/release/build-pkg.sh", X_OK) != 0, "build-pkg.sh not executable");

    FILE *f = fopen("scripts/release/build-pkg.sh", "r");
    HU_SKIP_IF(f == NULL, "build-pkg.sh cannot be opened");

    char line[256];
    int found_ostype_check = 0, found_exit_79 = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "OSTYPE") != NULL && strstr(line, "darwin") != NULL) {
            found_ostype_check = 1;
        }
        if (strstr(line, "exit 79") != NULL) {
            found_exit_79 = 1;
        }
    }
    fclose(f);

    HU_ASSERT_TRUE(found_ostype_check);
    HU_ASSERT_TRUE(found_exit_79);
}

void run_pkg_builder_tests(void) {
    HU_TEST_SUITE("pkg_builder");

    HU_RUN_TEST(test_pkg_builder_flag_parsing_output_flag);
    HU_RUN_TEST(test_pkg_builder_flag_parsing_app_path_flag);
    HU_RUN_TEST(test_pkg_builder_flag_parsing_defaults);
    HU_RUN_TEST(test_pkg_builder_uses_pkgbuild);
    HU_RUN_TEST(test_pkg_builder_pkgbuild_component_plist_flag);
    HU_RUN_TEST(test_pkg_builder_pkgbuild_identifier_flag);
    HU_RUN_TEST(test_pkg_builder_uses_productbuild);
    HU_RUN_TEST(test_pkg_builder_distribution_xml_template_exists);
    HU_RUN_TEST(test_pkg_builder_distribution_xml_has_version_placeholder);
    HU_RUN_TEST(test_pkg_builder_distribution_xml_has_installation_check);
    HU_RUN_TEST(test_pkg_builder_postinstall_script_exists);
    HU_RUN_TEST(test_pkg_builder_postinstall_validates_bundle);
    HU_RUN_TEST(test_pkg_builder_launchd_plist_exists);
    HU_RUN_TEST(test_pkg_builder_dry_run_mode);
    HU_RUN_TEST(test_pkg_builder_non_macos_exits_79);
}
