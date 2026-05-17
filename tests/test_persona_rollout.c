#include "test_framework.h"
#include "human/eval/persona_rollout.h"
#include "human/provider_test_seam.h"
#include "human/core/allocator.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static hu_communication_style_t make_target(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.sample_count = 1;
    s.lowercase_ratio = 0.8f;
    s.avg_message_length = 40.f;
    return s;
}

static void test_persona_rollout_rejects_null_inputs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_rollout_config_t cfg = {0};
    hu_persona_rollout_result_t out = {0};
    HU_ASSERT_EQ(hu_persona_rollout_run(&alloc, NULL, &out), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_rollout_run(&alloc, &cfg, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_persona_rollout_scores_in_unit_interval(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t *provider = NULL;
    HU_ASSERT_EQ(
        hu_provider_create_for_test_with_canned_response(&alloc, "canned: hey sounds good", &provider),
        HU_OK);

    const char *prompts[10] = {
        "p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8", "p9",
    };
    hu_communication_style_t target = make_target();
    hu_persona_rollout_config_t cfg = {
        .provider = provider,
        .target = &target,
        .prompts = prompts,
        .n_prompts = 10,
        .timeout_ms_per_prompt = 5000,
    };
    hu_persona_rollout_result_t rr = {0};
    HU_ASSERT_EQ(hu_persona_rollout_run(&alloc, &cfg, &rr), HU_OK);
    HU_ASSERT_EQ(rr.n_scored, 10);
    for (size_t i = 0; i < rr.n_scored; i++) {
        HU_ASSERT_TRUE(rr.persona_scores[i] >= 0.0);
        HU_ASSERT_TRUE(rr.persona_scores[i] <= 1.0);
    }
    HU_ASSERT_TRUE(rr.p95_ms >= 0.0);
    hu_persona_rollout_result_free(&alloc, &rr);
    hu_provider_destroy_for_test(provider, &alloc);
}

static void test_persona_rollout_load_fixture(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu-rollout-fixture-%d.txt", (int)getpid());
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    for (int i = 0; i < 10; i++)
        fprintf(f, "prompt line %d\n", i);
    fclose(f);

    char **lines = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_persona_rollout_load_prompt_fixture(&alloc, path, &lines, &n), HU_OK);
    HU_ASSERT_EQ(n, 10);
    for (size_t i = 0; i < n; i++)
        alloc.free(alloc.ctx, lines[i], strlen(lines[i]) + 1);
    alloc.free(alloc.ctx, lines, n * sizeof(char *));
    unlink(path);
}

void run_persona_rollout_tests(void) {
    HU_TEST_SUITE("persona-rollout");
    HU_RUN_TEST(test_persona_rollout_rejects_null_inputs);
    HU_RUN_TEST(test_persona_rollout_scores_in_unit_interval);
    HU_RUN_TEST(test_persona_rollout_load_fixture);
}
