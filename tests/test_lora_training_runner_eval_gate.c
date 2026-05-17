#include "test_framework.h"
#include "human/agent/lora_runner.h"
#include "human/agent/scheduler.h"
#include "human/eval/eval_gate.h"
#include "human/provider_test_seam.h"
#include "human/ml/learner.h"
#include "human/ml/learner_bridge.h"
#include "human/ml/fidelity.h"
#include "human/memory/personal_model.h"
#include "human/persona/persona_deltas.h"
#include "human/core/allocator.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool gate_test_setup_persona(char *tmpdir_out, size_t tmpdir_cap, const char *name) {
    char tmpl[] = "/tmp/human_gate_runner_test_XXXXXX";
    if (!mkdtemp(tmpl))
        return false;
    if (strlen(tmpl) + 1 > tmpdir_cap)
        return false;
    memcpy(tmpdir_out, tmpl, strlen(tmpl) + 1);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.json", tmpdir_out, name);
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return false;
    fprintf(fp,
            "{\n"
            "  \"version\": 1,\n"
            "  \"name\": \"%s\",\n"
            "  \"core\": {\"identity\": \"gate test persona\", \"traits\": [\"casual\"]},\n"
            "  \"example_banks\": [\n"
            "    {\"channel\": \"cli\", \"examples\": [\n"
            "      {\"context\": \"a\", \"incoming\": \"hi there\", \"response\": \"hey\"},\n"
            "      {\"context\": \"b\", \"incoming\": \"how are you\", \"response\": \"good u\"}\n"
            "    ]},\n"
            "    {\"channel\": \"telegram\", \"examples\": [\n"
            "      {\"context\": \"c\", \"incoming\": \"yo\", \"response\": \"ayy lmk\"}\n"
            "    ]}\n"
            "  ]\n"
            "}\n",
            name);
    fclose(fp);
    return true;
}

static void gate_test_cleanup(const char *tmpdir) {
    if (!tmpdir || !tmpdir[0])
        return;
    DIR *d = opendir(tmpdir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char p[1024];
            snprintf(p, sizeof(p), "%s/%s", tmpdir, de->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(tmpdir);
}

static float gate_test_expected_first_score(void) {
    hu_communication_style_t target;
    bool synthetic = true;
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_ml_fidelity_resolve_target(&alloc, &target, &synthetic), HU_OK);
    return hu_communication_style_fidelity_score_v2(&target, "hey", 3);
}

static void test_runner_skips_gate_when_eval_gate_is_null(void) {
    hu_learner_t *learner = NULL;
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_learner_open_default(&alloc, &learner), HU_OK);

    hu_lora_runner_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.learner = learner;
    ctx.alloc = &alloc;
    ctx.eval_gate = NULL;
    ctx.config_template = hu_learner_default_config();
    snprintf(ctx.config_template.adapter_output_path,
             sizeof(ctx.config_template.adapter_output_path),
             "/tmp/hu-gate-skip-%d.adapter", (int)getpid());

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &spec, 1000, &ctx), HU_OK);
    hu_learner_close(learner);
}

static void test_runner_blocks_promotion_when_gate_rejects(void) {
    char persona_dir[256];
    if (!gate_test_setup_persona(persona_dir, sizeof(persona_dir), "gate_reject"))
        return;
    setenv("HU_PERSONA_DIR", persona_dir, 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t *provider = NULL;
    HU_ASSERT_EQ(hu_provider_create_for_test_with_canned_response(&alloc, "canned: ok", &provider),
                 HU_OK);

    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.99,
        .persona_delta_min = 0.05,
        .bootstrap_samples = 100,
        .bootstrap_seed = 42,
    };

    hu_learner_t *learner = NULL;
    HU_ASSERT_EQ(hu_learner_open_default(&alloc, &learner), HU_OK);
    hu_persona_delta_t d;
    memset(&d, 0, sizeof(d));
    d.kind = HU_PERSONA_DELTA_TONE;
    snprintf(d.value, sizeof(d.value), "warm");
    HU_ASSERT_EQ(hu_learner_bridge_emit_persona_deltas(learner, &d, 1), HU_OK);

    char path[256];
    snprintf(path, sizeof(path), "/tmp/test-adapter-%d.lora", (int)getpid());
    unlink(path);
    unlink("/tmp/test-adapter.lora");
    unlink("/tmp/test-adapter.lora.rejected");

    hu_lora_runner_gate_capture_reset_for_test();
    hu_lora_runner_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.learner = learner;
    ctx.alloc = &alloc;
    ctx.provider = provider;
    ctx.eval_gate = &gate;
    ctx.rl_method_name = "dpo";
    ctx.persona_name = "gate_reject";
    ctx.config_template = hu_learner_default_config();
    snprintf(ctx.config_template.adapter_output_path,
             sizeof(ctx.config_template.adapter_output_path), "%s", path);

    setenv("HOME", "/tmp/test-home", 1);
    hu_lora_runner_set_test_clock(1747042800);

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &spec, 2000, &ctx), HU_OK);
    /* Gate measurement hot-loads the candidate adapter once; promotion
     * must not run after a reject. */
    HU_ASSERT_EQ(hu_provider_load_adapter_called_count_for_test(provider), 1);

    HU_ASSERT_TRUE(hu_lora_runner_gate_capture_n_for_test() >= 10);
    HU_ASSERT_TRUE(fabs(hu_lora_runner_gate_capture_persona_score_for_test(0) - 0.75) > 1e-6);
    float expected = gate_test_expected_first_score();
    HU_ASSERT_TRUE(expected >= 0.f);
    HU_ASSERT_TRUE(fabs((float)hu_lora_runner_gate_capture_persona_score_for_test(0) -
                        expected) < 1e-4f);
    HU_ASSERT_TRUE(hu_lora_runner_gate_capture_p95_ms_for_test() > 0.0);

    hu_learner_close(learner);
    hu_provider_destroy_for_test(provider, &alloc);
    gate_test_cleanup(persona_dir);
}

static void test_runner_gate_uses_measured_scores_not_synthetic_constant(void) {
    char persona_dir[256];
    if (!gate_test_setup_persona(persona_dir, sizeof(persona_dir), "gate_measure"))
        return;
    setenv("HU_PERSONA_DIR", persona_dir, 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t *provider = NULL;
    HU_ASSERT_EQ(hu_provider_create_for_test_with_canned_response(&alloc, "canned: ok", &provider),
                 HU_OK);

    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.40,
        .persona_delta_min = 0.01,
        .baseline_p95_latency_ms = 1000.0,
        .latency_delta_max_ms = 500.0,
        .bootstrap_samples = 100,
        .bootstrap_seed = 42,
    };

    hu_learner_t *learner = NULL;
    HU_ASSERT_EQ(hu_learner_open_default(&alloc, &learner), HU_OK);
    hu_persona_delta_t d;
    memset(&d, 0, sizeof(d));
    d.kind = HU_PERSONA_DELTA_TONE;
    snprintf(d.value, sizeof(d.value), "warm");
    HU_ASSERT_EQ(hu_learner_bridge_emit_persona_deltas(learner, &d, 1), HU_OK);

    char path[256];
    snprintf(path, sizeof(path), "/tmp/test-adapter-promote-%d.lora", (int)getpid());
    unlink(path);

    hu_lora_runner_gate_capture_reset_for_test();
    hu_lora_runner_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.learner = learner;
    ctx.alloc = &alloc;
    ctx.provider = provider;
    ctx.eval_gate = &gate;
    ctx.rl_method_name = "dpo";
    ctx.persona_name = "gate_measure";
    ctx.config_template = hu_learner_default_config();
    snprintf(ctx.config_template.adapter_output_path,
             sizeof(ctx.config_template.adapter_output_path), "%s", path);

    setenv("HOME", "/tmp/test-home-gate-promote", 1);
    hu_lora_runner_set_test_clock(1747042800);

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &spec, 2000, &ctx), HU_OK);

    size_t n = hu_lora_runner_gate_capture_n_for_test();
    HU_ASSERT_TRUE(n >= 10);
    bool any_not_075 = false;
    for (size_t i = 0; i < n; i++) {
        double s = hu_lora_runner_gate_capture_persona_score_for_test(i);
        HU_ASSERT_TRUE(s >= 0.0 && s <= 1.0);
        if (fabs(s - 0.75) > 1e-6)
            any_not_075 = true;
    }
    HU_ASSERT_TRUE(any_not_075);
    /* Once for gate eval, once for post-gate hot-load. */
    HU_ASSERT_TRUE(hu_provider_load_adapter_called_count_for_test(provider) >= 2);

    hu_learner_close(learner);
    hu_provider_destroy_for_test(provider, &alloc);
    gate_test_cleanup(persona_dir);
}

void run_runner_eval_gate_tests(void) {
    HU_TEST_SUITE("runner-eval-gate");
    HU_RUN_TEST(test_runner_skips_gate_when_eval_gate_is_null);
    HU_RUN_TEST(test_runner_blocks_promotion_when_gate_rejects);
    HU_RUN_TEST(test_runner_gate_uses_measured_scores_not_synthetic_constant);
}
