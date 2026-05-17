#include "test_framework.h"
#include "human/agent/lora_runner.h"
#include "human/agent/scheduler.h"
#include "human/eval/eval_gate.h"
#include "human/provider_test_seam.h"
#include "human/ml/learner.h"
#include "human/ml/learner_bridge.h"
#include "human/persona/persona_deltas.h"
#include "human/core/allocator.h"
#include <stdio.h>
#include <unistd.h>

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
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t *provider = NULL;
    HU_ASSERT_EQ(hu_provider_create_for_test_with_canned_response(&alloc, "canned: ok", &provider),
                 HU_OK);

    static const double low_scores[20] = {
        0.40, 0.40, 0.40, 0.40, 0.40, 0.40, 0.40, 0.40, 0.40, 0.40,
        0.40, 0.40, 0.40, 0.40, 0.40, 0.40, 0.40, 0.40, 0.40, 0.40,
    };

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

    hu_lora_runner_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.learner = learner;
    ctx.alloc = &alloc;
    ctx.provider = provider;
    ctx.eval_gate = &gate;
    ctx.gate_persona_after_scores = low_scores;
    ctx.gate_persona_after_n = 20;
    ctx.rl_method_name = "dpo";
    ctx.config_template = hu_learner_default_config();
    snprintf(ctx.config_template.adapter_output_path,
             sizeof(ctx.config_template.adapter_output_path), "%s", path);

    setenv("HOME", "/tmp/test-home", 1);
    hu_lora_runner_set_test_clock(1747042800);

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &spec, 2000, &ctx), HU_OK);
    HU_ASSERT_EQ(hu_provider_load_adapter_called_count_for_test(provider), 0);

    hu_learner_close(learner);
    hu_provider_destroy_for_test(provider, &alloc);
}

static void test_runner_promotes_measured_gate_scores(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t *provider = NULL;
    HU_ASSERT_EQ(hu_provider_create_for_test_with_canned_response(&alloc, "canned: ok", &provider),
                 HU_OK);

    static const double high_scores[20] = {
        0.82, 0.83, 0.84, 0.85, 0.86, 0.87, 0.88, 0.89, 0.90, 0.91,
        0.82, 0.83, 0.84, 0.85, 0.86, 0.87, 0.88, 0.89, 0.90, 0.91,
    };

    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.50,
        .persona_delta_min = 0.05,
        .bootstrap_samples = 200,
        .bootstrap_seed = 7,
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

    hu_lora_runner_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.learner = learner;
    ctx.alloc = &alloc;
    ctx.provider = provider;
    ctx.eval_gate = &gate;
    ctx.gate_persona_after_scores = high_scores;
    ctx.gate_persona_after_n = 20;
    ctx.rl_method_name = "dpo";
    ctx.config_template = hu_learner_default_config();
    snprintf(ctx.config_template.adapter_output_path,
             sizeof(ctx.config_template.adapter_output_path), "%s", path);

    setenv("HOME", "/tmp/test-home", 1);
    hu_lora_runner_set_test_clock(1747042800);

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &spec, 2000, &ctx), HU_OK);
    HU_ASSERT_EQ(hu_provider_load_adapter_called_count_for_test(provider), 1);

    hu_learner_close(learner);
    hu_provider_destroy_for_test(provider, &alloc);
    unlink(path);
}

void run_runner_eval_gate_tests(void) {
    HU_TEST_SUITE("runner-eval-gate");
    HU_RUN_TEST(test_runner_skips_gate_when_eval_gate_is_null);
    HU_RUN_TEST(test_runner_blocks_promotion_when_gate_rejects);
    HU_RUN_TEST(test_runner_promotes_measured_gate_scores);
}
