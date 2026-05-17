#include "test_framework.h"
#include "human/ml/cli_demo.h"
#include "human/core/allocator.h"
#include "human/core/json.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

static hu_error_t run_demo_to_dir(hu_allocator_t *alloc, const char *out) {
    const char *argv[] = {
        "human", "demo", "rl-closed-loop", "--backend", "huml", "--reaction-count", "50",
        "--prompt", "what should i do first?", "--out", out,
    };
    return hu_ml_cli_demo_rl_closed_loop((int)(sizeof(argv) / sizeof(argv[0])), argv, alloc);
}

static hu_json_value_t *read_json_file(hu_allocator_t *alloc, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    char *raw = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!raw) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[rd] = '\0';
    hu_json_value_t *jv = NULL;
    if (hu_json_parse(alloc, raw, rd, &jv) != HU_OK) {
        alloc->free(alloc->ctx, raw, (size_t)sz + 1);
        return NULL;
    }
    alloc->free(alloc->ctx, raw, (size_t)sz + 1);
    return jv;
}

static double json_object_get_number(hu_json_value_t *obj, const char *key) {
    if (!obj || obj->type != HU_JSON_OBJECT || !key)
        return -999.0;
    return hu_json_get_number(obj, key, -999.0);
}

static void test_demo_writes_nonstub_evidence_files(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char out[256];
    snprintf(out, sizeof(out), "/tmp/hu-demo-evidence-%d", (int)getpid());
    HU_ASSERT_EQ(run_demo_to_dir(&alloc, out), HU_OK);

    char path[512];
    const char *files[] = {"manifest.json",     "training_curves.json", "eval_before.json",
                           "eval_after.json",   "eval_delta.json",      "gate_decision.json",
                           "adversarial_review.md", "reproduce.sh",       "delta_responses.md"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", out, files[i]);
        HU_ASSERT_TRUE(file_size(path) > 50);
    }
}

static void test_demo_eval_delta_includes_bootstrap_ci(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char out[256];
    snprintf(out, sizeof(out), "/tmp/hu-demo-delta-%d", (int)getpid());
    HU_ASSERT_EQ(run_demo_to_dir(&alloc, out), HU_OK);

    char path[512];
    snprintf(path, sizeof(path), "%s/eval_delta.json", out);
    hu_json_value_t *jv = read_json_file(&alloc, path);
    HU_ASSERT_NOT_NULL(jv);
    HU_ASSERT_TRUE(json_object_get_number(jv, "bootstrap_p_value") >= 0.0);
    HU_ASSERT_TRUE(json_object_get_number(jv, "delta_mean") > -2.0);
    hu_json_free(&alloc, jv);
}

static void test_demo_gate_decision_has_real_verdict_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char out[256];
    snprintf(out, sizeof(out), "/tmp/hu-demo-gate-%d", (int)getpid());
    HU_ASSERT_EQ(run_demo_to_dir(&alloc, out), HU_OK);

    char path[512];
    snprintf(path, sizeof(path), "%s/gate_decision.json", out);
    char buf[1024];
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    HU_ASSERT_TRUE(strstr(buf, "promote") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"reason\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"demo\"") == NULL);
}

static void test_demo_training_curves_has_real_metrics(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char out[256];
    snprintf(out, sizeof(out), "/tmp/hu-demo-curves-%d", (int)getpid());
    HU_ASSERT_EQ(run_demo_to_dir(&alloc, out), HU_OK);

    char path[512];
    snprintf(path, sizeof(path), "%s/training_curves.json", out);
    hu_json_value_t *jv = read_json_file(&alloc, path);
    HU_ASSERT_NOT_NULL(jv);
    HU_ASSERT_TRUE(json_object_get_number(jv, "final_loss") >= 0.0);
    HU_ASSERT_TRUE(json_object_get_number(jv, "iters_completed") >= 0.0);
    hu_json_free(&alloc, jv);
}

static void test_demo_reproduce_sh_is_nonplaceholder(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char out[256];
    snprintf(out, sizeof(out), "/tmp/hu-demo-repro-%d", (int)getpid());
    HU_ASSERT_EQ(run_demo_to_dir(&alloc, out), HU_OK);

    char path[512];
    snprintf(path, sizeof(path), "%s/reproduce.sh", out);
    HU_ASSERT_TRUE(file_size(path) > 100);
    char buf[512];
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    HU_ASSERT_TRUE(strstr(buf, "cmake") != NULL || strstr(buf, "human demo") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "echo reproduce") == NULL);
}

static void test_demo_evidence_values_are_not_plan_example_literals(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char out[256];
    snprintf(out, sizeof(out), "/tmp/hu-demo-nolit-%d", (int)getpid());
    HU_ASSERT_EQ(run_demo_to_dir(&alloc, out), HU_OK);

    char path[512];
    snprintf(path, sizeof(path), "%s/eval_before.json", out);
    hu_json_value_t *before = read_json_file(&alloc, path);
    HU_ASSERT_NOT_NULL(before);
    double before_mean = json_object_get_number(before, "mean");
    hu_json_free(&alloc, before);

    snprintf(path, sizeof(path), "%s/eval_after.json", out);
    hu_json_value_t *after = read_json_file(&alloc, path);
    HU_ASSERT_NOT_NULL(after);
    double after_mean = json_object_get_number(after, "mean");
    hu_json_free(&alloc, after);

    snprintf(path, sizeof(path), "%s/eval_delta.json", out);
    hu_json_value_t *delta = read_json_file(&alloc, path);
    HU_ASSERT_NOT_NULL(delta);
    double delta_mean = json_object_get_number(delta, "delta_mean");
    hu_json_free(&alloc, delta);

    HU_ASSERT_TRUE(before_mean != 0.62);
    HU_ASSERT_TRUE(after_mean != 0.72);
    HU_ASSERT_TRUE(delta_mean != 0.10);
}

void run_cli_demo_evidence_tests(void) {
    HU_TEST_SUITE("cli-demo-evidence");
    HU_RUN_TEST(test_demo_writes_nonstub_evidence_files);
    HU_RUN_TEST(test_demo_eval_delta_includes_bootstrap_ci);
    HU_RUN_TEST(test_demo_gate_decision_has_real_verdict_fields);
    HU_RUN_TEST(test_demo_training_curves_has_real_metrics);
    HU_RUN_TEST(test_demo_reproduce_sh_is_nonplaceholder);
    HU_RUN_TEST(test_demo_evidence_values_are_not_plan_example_literals);
}
