/*
 * tests/test_install_docs.c
 * Test suite for installation guide documentation (US-C1.6)
 *
 * Validates:
 * - installation.md exists and is non-empty
 * - Required section headings are present
 * - Key concepts are documented (Gatekeeper, Full Disk Access, human doctor)
 * - README.md has been updated with Install section
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test_framework.h"

#define INSTALL_GUIDE_PATH "docs/guides/installation.md"
#define README_PATH        "README.md"

/* Helper: read entire file into allocated buffer */
static char *read_file_contents(const char *path, size_t *out_len) {
    struct stat st;
    if (stat(path, &st) < 0) {
        return NULL;
    }

    if (st.st_size == 0) {
        *out_len = 0;
        return calloc(1, 1);
    }

    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    char *buf = malloc(st.st_size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, st.st_size, f);
    fclose(f);

    if (nread != (size_t)st.st_size) {
        free(buf);
        return NULL;
    }

    buf[st.st_size] = '\0';
    *out_len = st.st_size;
    return buf;
}

/* Helper: case-insensitive substring search */
static int contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle)
        return 0;

    size_t needle_len = strlen(needle);
    for (size_t i = 0; haystack[i]; i++) {
        if (strncasecmp(&haystack[i], needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Test: installation.md exists and is non-empty */
static void test_installation_guide_exists(void) {
    size_t len = 0;
    char *contents = read_file_contents(INSTALL_GUIDE_PATH, &len);

    HU_ASSERT_NOT_NULL(contents);
    HU_ASSERT_GT(len, 250);

    free(contents);
}

/* Test: installation.md has required section headings */
static void test_installation_guide_has_required_sections(void) {
    size_t len = 0;
    char *contents = read_file_contents(INSTALL_GUIDE_PATH, &len);
    HU_ASSERT_NOT_NULL(contents);

    HU_ASSERT(strstr(contents, "# Installation Guide for Human") != NULL);
    HU_ASSERT(strstr(contents, "## Quick Start") != NULL);
    HU_ASSERT(strstr(contents, "## Gatekeeper") != NULL);
    HU_ASSERT(strstr(contents, "## Homebrew Installation") != NULL);
    HU_ASSERT(strstr(contents, "## Building from Source") != NULL);
    HU_ASSERT(strstr(contents, "## Troubleshooting") != NULL);
    HU_ASSERT(strstr(contents, "## Uninstalling Human") != NULL);

    free(contents);
}

/* Test: installation.md references "human doctor" with forward-ref note */
static void test_installation_guide_references_human_doctor(void) {
    size_t len = 0;
    char *contents = read_file_contents(INSTALL_GUIDE_PATH, &len);
    HU_ASSERT_NOT_NULL(contents);

    HU_ASSERT(contains_ci(contents, "human doctor"));

    HU_ASSERT(contains_ci(contents, "Sprint C3") || contains_ci(contents, "forthcoming") ||
              contains_ci(contents, "coming in"));

    free(contents);
}

/* Test: installation.md references Gatekeeper with Apple docs link */
static void test_installation_guide_explains_gatekeeper(void) {
    size_t len = 0;
    char *contents = read_file_contents(INSTALL_GUIDE_PATH, &len);
    HU_ASSERT_NOT_NULL(contents);

    HU_ASSERT(contains_ci(contents, "Gatekeeper"));
    HU_ASSERT(strstr(contents, "apple.com") != NULL ||
              strstr(contents, "support.apple.com") != NULL);

    free(contents);
}

/* Test: installation.md documents Full Disk Access and Accessibility */
static void test_installation_guide_documents_permissions(void) {
    size_t len = 0;
    char *contents = read_file_contents(INSTALL_GUIDE_PATH, &len);
    HU_ASSERT_NOT_NULL(contents);

    HU_ASSERT(contains_ci(contents, "Full Disk Access"));
    HU_ASSERT(contains_ci(contents, "Accessibility"));
    HU_ASSERT(contains_ci(contents, "System Settings") ||
              contains_ci(contents, "System Preferences"));

    free(contents);
}

/* Test: README.md has Install section above Build/Quick Start */
static void test_readme_has_install_section(void) {
    size_t len = 0;
    char *contents = read_file_contents(README_PATH, &len);
    HU_ASSERT_NOT_NULL(contents);

    HU_ASSERT(strstr(contents, "## Install") != NULL);
    HU_ASSERT(strstr(contents, "docs/guides/installation.md") != NULL);

    /* Verify Install comes before Quick Start */
    char *install_pos = strstr(contents, "## Install");
    char *quickstart_pos = strstr(contents, "## Quick Start");

    HU_ASSERT(install_pos != NULL && quickstart_pos != NULL);
    HU_ASSERT(install_pos < quickstart_pos);

    free(contents);
}

/* Test: placeholder image references exist as .txt markers */
static void test_installation_guide_image_placeholders_exist(void) {
    struct stat st;

    HU_ASSERT_EQ(stat("docs/guides/img/install-gatekeeper-prompt.png.txt", &st), 0);
    HU_ASSERT_EQ(stat("docs/guides/img/install-full-disk-access.png.txt", &st), 0);
    HU_ASSERT_EQ(stat("docs/guides/img/install-success.png.txt", &st), 0);
}

/* Test: basic markdown syntax check (backtick presence) */
static void test_installation_guide_markdown_syntax(void) {
    size_t len = 0;
    char *contents = read_file_contents(INSTALL_GUIDE_PATH, &len);
    HU_ASSERT_NOT_NULL(contents);

    int backtick_count = 0;
    for (size_t i = 0; i < len; i++) {
        if (contents[i] == '`') {
            backtick_count++;
        }
    }

    HU_ASSERT_GT(backtick_count, 10);

    free(contents);
}

void run_install_docs_tests(void) {
    HU_TEST_SUITE("InstallDocs");

    HU_RUN_TEST(test_installation_guide_exists);
    HU_RUN_TEST(test_installation_guide_has_required_sections);
    HU_RUN_TEST(test_installation_guide_references_human_doctor);
    HU_RUN_TEST(test_installation_guide_explains_gatekeeper);
    HU_RUN_TEST(test_installation_guide_documents_permissions);
    HU_RUN_TEST(test_readme_has_install_section);
    HU_RUN_TEST(test_installation_guide_image_placeholders_exist);
    HU_RUN_TEST(test_installation_guide_markdown_syntax);
}
