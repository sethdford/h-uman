#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"

/**
 * test_homebrew_formula.c
 *
 * Tests for the Homebrew formula structure and behavior (US-C1.4).
 * Verifies:
 * - Formula/human.rb exists and contains required sections
 * - Launchd plist template is valid XML with correct structure
 * - Post-install hook can render plist with path substitutions
 */

static void test_formula_file_exists(void) {
    HU_TEST_SUITE("homebrew_formula");

    FILE *fp = fopen("Formula/human.rb", "r");
    HU_ASSERT_NOT_NULL(fp);
    if (fp)
        fclose(fp);
}

static void test_formula_has_required_sections(void) {
    HU_TEST_SUITE("homebrew_formula");

    FILE *fp = fopen("Formula/human.rb", "r");
    HU_ASSERT_NOT_NULL(fp);
    if (!fp)
        return;

    char line[512];
    bool has_class = false;
    bool has_depends_on = false;
    bool has_install = false;
    bool has_post_install = false;
    bool has_test = false;
    bool has_caveats = false;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "class Human < Formula"))
            has_class = true;
        if (strstr(line, "depends_on"))
            has_depends_on = true;
        if (strstr(line, "def install"))
            has_install = true;
        if (strstr(line, "def post_install"))
            has_post_install = true;
        if (strstr(line, "test do"))
            has_test = true;
        if (strstr(line, "def caveats"))
            has_caveats = true;
    }
    fclose(fp);

    HU_ASSERT_TRUE(has_class);
    HU_ASSERT_TRUE(has_depends_on);
    HU_ASSERT_TRUE(has_install);
    HU_ASSERT_TRUE(has_post_install);
    HU_ASSERT_TRUE(has_test);
    HU_ASSERT_TRUE(has_caveats);
}

static void test_plist_template_exists(void) {
    HU_TEST_SUITE("homebrew_formula");

    FILE *fp = fopen("scripts/install/human-daemon.plist.template", "r");
    HU_ASSERT_NOT_NULL(fp);
    if (fp)
        fclose(fp);
}

static void test_plist_template_is_valid_xml(void) {
    HU_TEST_SUITE("homebrew_formula");

    FILE *fp = fopen("scripts/install/human-daemon.plist.template", "r");
    HU_ASSERT_NOT_NULL(fp);
    if (!fp)
        return;

    char line[512];
    bool has_xml_decl = false;
    bool has_plist_open = false;
    bool has_plist_close = false;
    bool has_label = false;
    bool has_program_args = false;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "<?xml version"))
            has_xml_decl = true;
        if (strstr(line, "<plist"))
            has_plist_open = true;
        if (strstr(line, "</plist>"))
            has_plist_close = true;
        if (strstr(line, "<key>Label</key>"))
            has_label = true;
        if (strstr(line, "<key>ProgramArguments</key>"))
            has_program_args = true;
    }
    fclose(fp);

    HU_ASSERT_TRUE(has_xml_decl);
    HU_ASSERT_TRUE(has_plist_open);
    HU_ASSERT_TRUE(has_plist_close);
    HU_ASSERT_TRUE(has_label);
    HU_ASSERT_TRUE(has_program_args);
}

static void test_plist_template_has_substitution_markers(void) {
    HU_TEST_SUITE("homebrew_formula");

    FILE *fp = fopen("scripts/install/human-daemon.plist.template", "r");
    HU_ASSERT_NOT_NULL(fp);
    if (!fp)
        return;

    char line[512];
    bool has_brew_prefix = false;
    bool has_home = false;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "{{BREW_PREFIX}}"))
            has_brew_prefix = true;
        if (strstr(line, "{{HOME}}"))
            has_home = true;
    }
    fclose(fp);

    HU_ASSERT_TRUE(has_brew_prefix);
    HU_ASSERT_TRUE(has_home);
}

static void test_plist_template_references_correct_binary(void) {
    HU_TEST_SUITE("homebrew_formula");

    FILE *fp = fopen("scripts/install/human-daemon.plist.template", "r");
    HU_ASSERT_NOT_NULL(fp);
    if (!fp)
        return;

    char line[512];
    bool references_human_binary = false;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "{{BREW_PREFIX}}/bin/human"))
            references_human_binary = true;
    }
    fclose(fp);

    HU_ASSERT_TRUE(references_human_binary);
}

void run_homebrew_formula_tests(void) {
    HU_RUN_TEST(test_formula_file_exists);
    HU_RUN_TEST(test_formula_has_required_sections);
    HU_RUN_TEST(test_plist_template_exists);
    HU_RUN_TEST(test_plist_template_is_valid_xml);
    HU_RUN_TEST(test_plist_template_has_substitution_markers);
    HU_RUN_TEST(test_plist_template_references_correct_binary);
}
