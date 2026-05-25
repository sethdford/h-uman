/* Tests for diagnose-notary.sh — notarization failure translator */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DIAGNOSE_SCRIPT "./scripts/release/diagnose-notary.sh"

static char *run_diagnose(const char *fixture_path, int *exit_code) {
    /* Invoke diagnose-notary.sh and capture stdout + stderr */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s --log %s 2>&1", DIAGNOSE_SCRIPT, fixture_path);

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        return NULL;
    }

    /* Read output into a buffer */
    char *output = malloc(4096);
    if (!output) {
        pclose(pipe);
        return NULL;
    }

    size_t n = 0;
    int ch;
    while ((ch = fgetc(pipe)) != EOF && n < 4095) {
        output[n++] = (char)ch;
    }
    output[n] = '\0';

    *exit_code = pclose(pipe) >> 8; /* Extract exit code from wait status */
    return output;
}

static void test_diagnose_sdk_too_old(void) {
    int exit_code = -1;
    char *output = run_diagnose("tests/fixtures/notary/sdk-too-old.json", &exit_code);
    HU_ASSERT(output != NULL);
    HU_ASSERT(exit_code == 1); /* One issue found */
    HU_ASSERT_STR_CONTAINS(output, "SDK");
    HU_ASSERT_STR_CONTAINS(output, "rebuild");
    HU_ASSERT_STR_CONTAINS(output, "macOS SDK");
    free(output);
}

static void test_diagnose_missing_timestamp(void) {
    int exit_code = -1;
    char *output = run_diagnose("tests/fixtures/notary/missing-timestamp.json", &exit_code);
    HU_ASSERT(output != NULL);
    HU_ASSERT(exit_code == 1); /* One issue found */
    HU_ASSERT_STR_CONTAINS(output, "timestamp");
    HU_ASSERT_STR_CONTAINS(output, "codesign");
    free(output);
}

static void test_diagnose_missing_hardened_runtime(void) {
    int exit_code = -1;
    char *output = run_diagnose("tests/fixtures/notary/missing-hardened-runtime.json", &exit_code);
    HU_ASSERT(output != NULL);
    HU_ASSERT(exit_code == 1); /* One issue found */
    HU_ASSERT_STR_CONTAINS(output, "hardened");
    HU_ASSERT_STR_CONTAINS(output, "runtime");
    free(output);
}

static void test_diagnose_unknown_issue(void) {
    int exit_code = -1;
    char *output = run_diagnose("tests/fixtures/notary/unknown-issue.json", &exit_code);
    HU_ASSERT(output != NULL);
    HU_ASSERT(exit_code == 1); /* One issue found */
    /* Unknown issues should reference Apple's docs or notarization guidance */
    if (!strstr(output, "notarization") && !strstr(output, "Apple") && !strstr(output, "apple")) {
        HU_FAIL("Unknown issue should reference Apple or notarization in output");
    }
    free(output);
}

static void test_diagnose_clean(void) {
    int exit_code = -1;
    char *output = run_diagnose("tests/fixtures/notary/clean.json", &exit_code);
    HU_ASSERT(output != NULL);
    HU_ASSERT(exit_code == 0); /* No issues */
    HU_ASSERT_STR_CONTAINS(output, "CLEAN");
    free(output);
}

static void test_diagnose_missing_file(void) {
    int exit_code = -1;
    char *output = run_diagnose("/nonexistent/path/notary.json", &exit_code);
    HU_ASSERT(output != NULL);
    HU_ASSERT(exit_code != 0); /* Should fail */
    if (!strstr(output, "not found") && !strstr(output, "Error")) {
        HU_FAIL("Error message should mention file not found or Error");
    }
    free(output);
}

static void test_diagnose_help_flag(void) {
    /* Test --help flag */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s --help 2>&1", DIAGNOSE_SCRIPT);

    FILE *pipe = popen(cmd, "r");
    HU_ASSERT(pipe != NULL);

    char output[1024] = {0};
    size_t n = 0;
    int ch;
    while ((ch = fgetc(pipe)) != EOF && n < sizeof(output) - 1) {
        output[n++] = (char)ch;
    }
    output[n] = '\0';

    int exit_code = pclose(pipe) >> 8;
    HU_ASSERT(exit_code == 0); /* --help should succeed */
    if (!strstr(output, "Usage") && !strstr(output, "usage")) {
        HU_FAIL("Help output should contain 'Usage' or 'usage'");
    }
}

void run_diagnose_notary_tests(void) {
    HU_TEST_SUITE("diagnose_notary");
    HU_RUN_TEST(test_diagnose_sdk_too_old);
    HU_RUN_TEST(test_diagnose_missing_timestamp);
    HU_RUN_TEST(test_diagnose_missing_hardened_runtime);
    HU_RUN_TEST(test_diagnose_unknown_issue);
    HU_RUN_TEST(test_diagnose_clean);
    HU_RUN_TEST(test_diagnose_missing_file);
    HU_RUN_TEST(test_diagnose_help_flag);
}
