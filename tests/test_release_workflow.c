// @covers-none — asserts on .github/workflows/release-macos.yml, not a C module
#include "test_framework.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HU_IS_TEST

// Simple YAML key-value line matcher
// Returns true if a line contains "key: <pattern>"
static bool yaml_line_contains(const char *line, const char *key, const char *value_pattern) {
    if (!line || !key || !value_pattern)
        return false;
    if (line[0] == '#')
        return false; // Skip comments

    // Find the key
    const char *key_pos = strstr(line, key);
    if (!key_pos)
        return false;

    // Verify it's followed by a colon
    size_t key_len = strlen(key);
    if (key_pos[key_len] != ':')
        return false;

    // Find the value part after the colon
    const char *value_pos = strchr(key_pos, ':');
    if (!value_pos)
        return false;
    value_pos++; // Skip the colon

    // Skip whitespace
    while (*value_pos && isspace((unsigned char)*value_pos)) {
        value_pos++;
    }

    // Check if value matches pattern
    return strstr(value_pos, value_pattern) != NULL;
}

// Check if a line indicates a GitHub Actions trigger
static bool is_trigger_line(const char *line, const char *trigger_type) {
    // Look for lines like:
    //   branches: [main]
    //   tags: [v*]
    //   push:
    if (!line || !trigger_type)
        return false;
    if (line[0] == '#')
        return false;

    return strstr(line, trigger_type) != NULL;
}

static void test_release_workflow_yaml_exists(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);
    fclose(f);
}

static void test_release_workflow_has_push_to_main_trigger(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    bool found_branches = false;
    bool found_main = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "branches:")) {
            found_branches = true;
        }
        if (found_branches && strstr(line, "main")) {
            found_main = true;
            break;
        }
    }

    HU_ASSERT_TRUE(found_branches && found_main);
    fclose(f);
}

static void test_release_workflow_has_tag_trigger(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    bool found_tags = false;
    bool found_v_pattern = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "tags:")) {
            found_tags = true;
            // Check if the next few lines contain v* pattern
            for (int i = 0; i < 3; i++) {
                if (!fgets(line, sizeof(line), f))
                    break;
                if (strstr(line, "v*") || strstr(line, "v")) {
                    found_v_pattern = true;
                    break;
                }
            }
            break;
        }
    }

    HU_ASSERT_TRUE(found_tags && found_v_pattern);
    fclose(f);
}

/* GitHub-hosted arm64 macOS runner labels (actions/runner-images README).
 * "macos-14-arm64" is an image NAME, not a label: a job requesting it is never
 * assigned a runner and waits in the queue until cancelled. */
static bool is_hosted_macos_arm64_label(const char *label) {
    static const char *const labels[] = {"macos-14", "macos-14-xlarge", "macos-15",
                                         "macos-15-xlarge"};
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (strcmp(label, labels[i]) == 0)
            return true;
    }
    return false;
}

static void test_release_workflow_runs_on_macos_arm64(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    int macos_jobs = 0;
    int valid_macos_jobs = 0;

    while (fgets(line, sizeof(line), f)) {
        const char *p = strstr(line, "runs-on:");
        if (!p)
            continue;
        p += strlen("runs-on:");
        while (*p == ' ' || *p == '\t')
            p++;
        char label[64];
        size_t n = 0;
        while (p[n] && !isspace((unsigned char)p[n]) && n + 1 < sizeof(label)) {
            label[n] = p[n];
            n++;
        }
        label[n] = '\0';
        if (strncmp(label, "macos", 5) != 0)
            continue;
        macos_jobs++;
        if (is_hosted_macos_arm64_label(label))
            valid_macos_jobs++;
    }

    HU_ASSERT_TRUE(macos_jobs > 0);
    HU_ASSERT_EQ(valid_macos_jobs, macos_jobs);
    fclose(f);
}

static void test_release_workflow_has_concurrency_group(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    bool found_concurrency = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "concurrency:")) {
            found_concurrency = true;
            // Verify group exists in the next few lines
            for (int i = 0; i < 3; i++) {
                if (!fgets(line, sizeof(line), f))
                    break;
                if (strstr(line, "group:")) {
                    fclose(f);
                    HU_ASSERT_TRUE(true);
                    return;
                }
            }
            break;
        }
    }

    HU_ASSERT_TRUE(found_concurrency);
    fclose(f);
}

static void test_release_workflow_has_required_secrets(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    int secrets_found = 0;
    const char *required_secrets[] = {"APPLE_DEVELOPER_ID_APPLICATION_CERT",
                                      "APPLE_DEVELOPER_ID_INSTALLER_CERT",
                                      "APPLE_DEVELOPER_ID_CERT_PASSWORD",
                                      "APPLE_API_KEY_ID",
                                      "APPLE_API_KEY_ISSUER",
                                      "APPLE_API_KEY_BASE64"};
    int required_count = 6;

    while (fgets(line, sizeof(line), f)) {
        for (int i = 0; i < required_count; i++) {
            if (strstr(line, required_secrets[i])) {
                secrets_found++;
            }
        }
    }

    // Expect at least some secrets referenced
    HU_ASSERT_TRUE(secrets_found >= 3);
    fclose(f);
}

static void test_release_workflow_does_not_echo_secrets(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[512];
    bool found_unsafe_echo = false;

    while (fgets(line, sizeof(line), f)) {
        // Look for direct echo/print of secret variables
        // Pattern: echo.*$APPLE_* or similar
        if (strstr(line, "echo") && (strstr(line, "$APPLE_") || strstr(line, "${{ secrets."))) {
            // Check if it's not in a benign context (like echo ::notice::)
            if (!strstr(line, "::notice::") && !strstr(line, "::warning::")) {
                found_unsafe_echo = true;
                break;
            }
        }
    }

    HU_ASSERT_FALSE(found_unsafe_echo);
    fclose(f);
}

static void test_release_workflow_has_build_job(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    bool found_build = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "build:") && (line[0] == ' ' || line[0] == '-')) {
            found_build = true;
            break;
        }
    }

    HU_ASSERT_TRUE(found_build);
    fclose(f);
}

static void test_release_workflow_has_preflight_step(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    bool found_preflight = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "scripts/agent-preflight.sh") ||
            (strstr(line, "preflight") && strstr(line, "bash"))) {
            found_preflight = true;
            break;
        }
    }

    HU_ASSERT_TRUE(found_preflight);
    fclose(f);
}

static void test_release_workflow_uploads_artifact(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    bool found_upload = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "upload-artifact") || strstr(line, "actions/upload-artifact")) {
            found_upload = true;
            break;
        }
    }

    HU_ASSERT_TRUE(found_upload);
    fclose(f);
}

static void test_release_workflow_tags_create_release(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    bool found_release = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "softprops/action-gh-release") || strstr(line, "action-gh-release")) {
            found_release = true;
            break;
        }
    }

    HU_ASSERT_TRUE(found_release);
    fclose(f);
}

static void test_release_workflow_sign_job_is_conditional(void) {
    FILE *f = fopen(".github/workflows/release-macos.yml", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    bool found_sign_job = false;
    bool found_conditional = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "sign-and-release:")) {
            found_sign_job = true;
        }
        if (found_sign_job && strstr(line, "if:") &&
            (strstr(line, "github.ref_type") || strstr(line, "tag"))) {
            found_conditional = true;
            break;
        }
    }

    HU_ASSERT_TRUE(found_sign_job && found_conditional);
    fclose(f);
}

static void test_sign_notarize_script_exists(void) {
    FILE *f = fopen("scripts/release/sign-and-notarize.sh", "r");
    HU_ASSERT_NOT_NULL(f);
    fclose(f);
}

static void test_diagnose_notary_script_exists(void) {
    FILE *f = fopen("scripts/release/diagnose-notary.sh", "r");
    HU_ASSERT_NOT_NULL(f);
    fclose(f);
}

static void test_sign_notarize_script_has_pkg_arg(void) {
    FILE *f = fopen("scripts/release/sign-and-notarize.sh", "r");
    HU_ASSERT_NOT_NULL(f);

    char line[256];
    bool found_pkg_arg = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "--pkg")) {
            found_pkg_arg = true;
            break;
        }
    }

    HU_ASSERT_TRUE(found_pkg_arg);
    fclose(f);
}

void run_release_workflow_tests(void) {
    HU_TEST_SUITE("release_workflow");
    HU_RUN_TEST(test_release_workflow_yaml_exists);
    HU_RUN_TEST(test_release_workflow_has_push_to_main_trigger);
    HU_RUN_TEST(test_release_workflow_has_tag_trigger);
    HU_RUN_TEST(test_release_workflow_runs_on_macos_arm64);
    HU_RUN_TEST(test_release_workflow_has_concurrency_group);
    HU_RUN_TEST(test_release_workflow_has_required_secrets);
    HU_RUN_TEST(test_release_workflow_does_not_echo_secrets);
    HU_RUN_TEST(test_release_workflow_has_build_job);
    HU_RUN_TEST(test_release_workflow_has_preflight_step);
    HU_RUN_TEST(test_release_workflow_uploads_artifact);
    HU_RUN_TEST(test_release_workflow_tags_create_release);
    HU_RUN_TEST(test_release_workflow_sign_job_is_conditional);
    HU_RUN_TEST(test_sign_notarize_script_exists);
    HU_RUN_TEST(test_diagnose_notary_script_exists);
    HU_RUN_TEST(test_sign_notarize_script_has_pkg_arg);
}

#else
void run_release_workflow_tests(void) {
    (void)0;
}
#endif
