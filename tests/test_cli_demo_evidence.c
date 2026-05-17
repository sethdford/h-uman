/* tests/test_cli_demo_evidence.c — CF-2 wiring tests
 *
 * Pins that `human demo rl-closed-loop --out <dir>` writes real,
 * non-stub content to all nine evidence files (the original Phase 6
 * implementation wrote 6 of 9 as `{}\n`):
 *
 *   manifest.json           — real persona_delta (not 0.06 literal)
 *   training_curves.json    — real trainer metrics
 *   eval_before.json        — real per-conversation score array
 *   eval_after.json         — real per-conversation score array
 *   eval_delta.json         — real before/after means + bootstrap p
 *   gate_decision.json      — real hu_eval_gate verdict
 *   adversarial_review.md   — structured automated review
 *   delta_responses.md      — real before/after responses + delta
 *   reproduce.sh            — real shell snippet, not `echo reproduce`
 */

#include "test_framework.h"
#include "human/ml/cli_demo.h"
#include "human/core/allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static size_t read_file_into(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t r = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[r] = '\0';
    return r;
}

static hu_error_t run_demo_into(const char *out_dir) {
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "--reaction-count";
    char a1[] = "20";
    char a2[] = "--backend";
    char a3[] = "huml";
    char a4[] = "--out";
    char a5[512];
    snprintf(a5, sizeof(a5), "%s", out_dir);
    const char *argv[] = {a0, a1, a2, a3, a4, a5};
    return hu_ml_cli_demo_rl_closed_loop(6, argv, &alloc);
}

static void test_demo_manifest_has_measured_persona_delta_not_literal(void) {
    const char *dir = "/tmp/cf2-evidence-manifest";
    HU_ASSERT_EQ(run_demo_into(dir), HU_OK);
    char path[256];
    snprintf(path, sizeof(path), "%s/manifest.json", dir);

    char buf[4096];
    size_t r = read_file_into(path, buf, sizeof(buf));
    HU_ASSERT_TRUE(r > 100);

    /* The original implementation hard-coded persona_delta=0.06 and
     * had no persona_delta_source / trainer / synthetic fields. */
    HU_ASSERT_TRUE(strstr(buf, "persona_delta_source") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "after_mean - before_mean") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "trainer") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "synthetic") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "0.0600") == NULL);
}

static void test_demo_training_curves_json_is_real(void) {
    const char *dir = "/tmp/cf2-evidence-curves";
    HU_ASSERT_EQ(run_demo_into(dir), HU_OK);
    char path[256];
    snprintf(path, sizeof(path), "%s/training_curves.json", dir);

    char buf[2048];
    size_t r = read_file_into(path, buf, sizeof(buf));
    HU_ASSERT_TRUE(r > 50);
    HU_ASSERT_TRUE(strcmp(buf, "{}\n") != 0);
    HU_ASSERT_TRUE(strstr(buf, "iters_completed") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "final_loss") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "chosen_logprob_delta") != NULL);
}

static void test_demo_eval_before_and_after_have_score_arrays(void) {
    const char *dir = "/tmp/cf2-evidence-scores";
    HU_ASSERT_EQ(run_demo_into(dir), HU_OK);

    char path[256];
    char buf[8192];

    snprintf(path, sizeof(path), "%s/eval_before.json", dir);
    HU_ASSERT_TRUE(read_file_into(path, buf, sizeof(buf)) > 100);
    HU_ASSERT_TRUE(strcmp(buf, "{}\n") != 0);
    HU_ASSERT_TRUE(strstr(buf, "persona_fidelity_before") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"scores\":[") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"n\":20") != NULL);

    snprintf(path, sizeof(path), "%s/eval_after.json", dir);
    HU_ASSERT_TRUE(read_file_into(path, buf, sizeof(buf)) > 100);
    HU_ASSERT_TRUE(strcmp(buf, "{}\n") != 0);
    HU_ASSERT_TRUE(strstr(buf, "persona_fidelity_after") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"n\":20") != NULL);
}

static void test_demo_eval_delta_has_real_bootstrap_p_value(void) {
    const char *dir = "/tmp/cf2-evidence-delta";
    HU_ASSERT_EQ(run_demo_into(dir), HU_OK);
    char path[256];
    snprintf(path, sizeof(path), "%s/eval_delta.json", dir);

    char buf[1024];
    HU_ASSERT_TRUE(read_file_into(path, buf, sizeof(buf)) > 50);
    HU_ASSERT_TRUE(strcmp(buf, "{}\n") != 0);
    HU_ASSERT_TRUE(strstr(buf, "before_mean") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "after_mean") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "bootstrap_p_value") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "delta_mean") != NULL);
}

static void test_demo_gate_decision_uses_real_eval_gate(void) {
    const char *dir = "/tmp/cf2-evidence-gate";
    HU_ASSERT_EQ(run_demo_into(dir), HU_OK);
    char path[256];
    snprintf(path, sizeof(path), "%s/gate_decision.json", dir);

    char buf[2048];
    HU_ASSERT_TRUE(read_file_into(path, buf, sizeof(buf)) > 30);
    /* Original stub: `{"promote":true,"reason":"demo"}` -- pin that
     * the new content includes the real gate's CI fields or the
     * fallback marker, never the literal "reason":"demo". */
    HU_ASSERT_TRUE(strcmp(buf, "{}\n") != 0);
    HU_ASSERT_TRUE(strstr(buf, "\"reason\":\"demo\"") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "promote") != NULL);
    /* With 20 reactions (>=10), the gate ran. */
    HU_ASSERT_TRUE(strstr(buf, "persona_ci_lower") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "hu_eval_gate") != NULL);
}

static void test_demo_adversarial_review_has_structured_body(void) {
    const char *dir = "/tmp/cf2-evidence-review";
    HU_ASSERT_EQ(run_demo_into(dir), HU_OK);
    char path[256];
    snprintf(path, sizeof(path), "%s/adversarial_review.md", dir);

    char buf[8192];
    HU_ASSERT_TRUE(read_file_into(path, buf, sizeof(buf)) > 200);
    HU_ASSERT_TRUE(strcmp(buf, "{}\n") != 0);
    HU_ASSERT_TRUE(strstr(buf, "Automated adversarial review") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Trainer") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Eval gate") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Caveats") != NULL);
}

static void test_demo_reproduce_sh_is_real_command_not_placeholder(void) {
    const char *dir = "/tmp/cf2-evidence-repro";
    HU_ASSERT_EQ(run_demo_into(dir), HU_OK);
    char path[256];
    snprintf(path, sizeof(path), "%s/reproduce.sh", dir);

    char buf[2048];
    HU_ASSERT_TRUE(read_file_into(path, buf, sizeof(buf)) > 100);
    /* Original stub: "#!/bin/sh\necho reproduce\n" (25 bytes). */
    HU_ASSERT_TRUE(strstr(buf, "echo reproduce") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "human demo rl-closed-loop") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "--reaction-count 20") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "--backend huml") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "HU_E2E_FIXED_TIMESTAMP") != NULL);
}

static void test_demo_delta_responses_md_has_real_delta(void) {
    const char *dir = "/tmp/cf2-evidence-delta-md";
    HU_ASSERT_EQ(run_demo_into(dir), HU_OK);
    char path[256];
    snprintf(path, sizeof(path), "%s/delta_responses.md", dir);

    char buf[1024];
    HU_ASSERT_TRUE(read_file_into(path, buf, sizeof(buf)) > 80);
    HU_ASSERT_TRUE(strstr(buf, "Persona-fidelity delta") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "bootstrap p=") != NULL);
}

static void test_demo_all_nine_files_exceed_stub_size(void) {
    const char *dir = "/tmp/cf2-evidence-sizes";
    HU_ASSERT_EQ(run_demo_into(dir), HU_OK);
    /* Original sizes were: 3 bytes for the five `{}\n` files,
     * 25 bytes for `reproduce.sh`, ~115 bytes for `manifest.json`,
     * ~33 bytes for `gate_decision.json`, ~96 bytes for
     * `delta_responses.md`. Every file should now exceed 50 bytes. */
    const char *files[] = {
        "manifest.json",       "training_curves.json", "eval_before.json",
        "eval_after.json",     "eval_delta.json",      "gate_decision.json",
        "adversarial_review.md","delta_responses.md",  "reproduce.sh",
    };
    char path[256];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        long sz = file_size(path);
        if (sz < 50) {
            HU_FAIL("CF-2 stub regression: %s is %ld bytes (expected >= 50)",
                    files[i], sz);
        }
    }
}

void run_cli_demo_evidence_tests(void) {
    HU_TEST_SUITE("cli-demo-evidence");
    HU_RUN_TEST(test_demo_manifest_has_measured_persona_delta_not_literal);
    HU_RUN_TEST(test_demo_training_curves_json_is_real);
    HU_RUN_TEST(test_demo_eval_before_and_after_have_score_arrays);
    HU_RUN_TEST(test_demo_eval_delta_has_real_bootstrap_p_value);
    HU_RUN_TEST(test_demo_gate_decision_uses_real_eval_gate);
    HU_RUN_TEST(test_demo_adversarial_review_has_structured_body);
    HU_RUN_TEST(test_demo_reproduce_sh_is_real_command_not_placeholder);
    HU_RUN_TEST(test_demo_delta_responses_md_has_real_delta);
    HU_RUN_TEST(test_demo_all_nine_files_exceed_stub_size);
}
