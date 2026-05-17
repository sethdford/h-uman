#include "test_framework.h"
#include "human/agent/adapter_id.h"
#include "human/agent/lora_runner.h"
#include "human/agent/scheduler.h"
#include "human/eval/eval_gate.h"
#include "human/ml/learner.h"
#include "human/ml/learner_bridge.h"
#include "human/persona/persona_deltas.h"
#include "human/core/allocator.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_proof_directory_gate_decision_json_only_on_reject(void) {
    hu_allocator_t alloc = hu_system_allocator();
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
    snprintf(d.value, sizeof(d.value), "x");
    HU_ASSERT_EQ(hu_learner_bridge_emit_persona_deltas(learner, &d, 1), HU_OK);

    char path[256];
    snprintf(path, sizeof(path), "/tmp/test-adapter-%d.lora", (int)getpid());

    hu_lora_runner_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.learner = learner;
    ctx.alloc = &alloc;
    ctx.eval_gate = &gate;
    ctx.rl_method_name = "dpo";
    ctx.config_template = hu_learner_default_config();
    snprintf(ctx.config_template.adapter_output_path,
             sizeof(ctx.config_template.adapter_output_path), "%s", path);

    setenv("HOME", "/tmp/test-home", 1);
    hu_lora_runner_set_test_clock(1747042800);
    (void)mkdir("/tmp/test-home", 0755);
    (void)mkdir("/tmp/test-home/.human", 0755);
    (void)mkdir("/tmp/test-home/.human/proofs", 0755);

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &spec, 2000, &ctx), HU_OK);

    char adapter_id[128];
    HU_ASSERT_EQ(hu_format_adapter_id("dpo", 0, 1747042800, adapter_id, sizeof(adapter_id)),
                 HU_OK);
    char proof_gate[512];
    snprintf(proof_gate, sizeof(proof_gate), "/tmp/test-home/.human/proofs/%s/gate_decision.json",
             adapter_id);
    HU_ASSERT_EQ(access(proof_gate, F_OK), 0);
    char proof_manifest[512];
    snprintf(proof_manifest, sizeof(proof_manifest),
             "/tmp/test-home/.human/proofs/%s/manifest.json", adapter_id);
    HU_ASSERT_EQ(access(proof_manifest, F_OK), -1);

    hu_learner_close(learner);
}

void run_proof_directory_tests(void) {
    HU_TEST_SUITE("proof-directory");
    HU_RUN_TEST(test_proof_directory_gate_decision_json_only_on_reject);
}
