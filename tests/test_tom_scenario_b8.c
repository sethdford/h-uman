#include "human/agent/tom_scenario.h"
#include "human/core/allocator.h"
#include "test_framework.h"

#include <string.h>

#ifndef HU_EVAL_SUITES_DIR
#error "HU_EVAL_SUITES_DIR must be defined when building human_tests"
#endif

static void tom_scenario_synthesize_false_belief_tag(void) {
    hu_theory_of_mind_t tom;
    const char *p = "Max did not see the move.";
    const char *q = "Where will Max look?";
    hu_tom_scenario_synthesize(p, strlen(p), q, strlen(q), "false_belief", 12, 1000LL, &tom);
    HU_ASSERT_NOT_NULL(strstr(tom.user_expects_we_cannot, "[ToM:fb]"));
    HU_ASSERT_NOT_NULL(strstr(tom.user_thinks_we_are, "Max"));
    HU_ASSERT_NOT_NULL(strstr(tom.user_expects_we_can, "Where"));
}

static void tom_gold_matches_response_handles_underscore_tokens(void) {
    const char *s1 = "Max will search the original basket first because he did not see the move.";
    HU_ASSERT_TRUE(hu_tom_scenario_gold_matches_response("original_basket", s1, strlen(s1), 3));
    const char *s2 = "only the drawer.";
    HU_ASSERT_TRUE(!hu_tom_scenario_gold_matches_response("original_basket", s2, strlen(s2), 3));
    const char *s3 = "Sam thinks the cookies are in the jar.";
    HU_ASSERT_TRUE(hu_tom_scenario_gold_matches_response("jar", s3, strlen(s3), 3));
}

static void tom_b8_repo_pack_gold_scores_partial_coverage(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[768];
    int n = snprintf(path, sizeof(path), "%s/tom/tom_synthetic.json", HU_EVAL_SUITES_DIR);
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(path));
    unsigned pass = 0, total = 0;
    HU_ASSERT_EQ(hu_tom_b8_synthetic_pack_score_gold(&alloc, path, &pass, &total), HU_OK);
    HU_ASSERT_EQ(total, 10u);
    /* Many gold keys are answer rubrics, not substrings of the vignette; we
     * only assert a stable lower bound on premise+question+stub overlap. */
    HU_ASSERT_GE(pass, 3u);
}

static void tom_b8_repo_pack_all_items_pass_smoke(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[768];
    int n = snprintf(path, sizeof(path), "%s/tom/tom_synthetic.json", HU_EVAL_SUITES_DIR);
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(path));
    unsigned pass = 0, total = 0;
    HU_ASSERT_EQ(hu_tom_b8_synthetic_pack_run_smoke(&alloc, path, &pass, &total), HU_OK);
    HU_ASSERT_EQ(total, 10u);
    HU_ASSERT_EQ(pass, 10u);
}

void run_tom_scenario_tests(void);

void run_tom_scenario_tests(void) {
    HU_TEST_SUITE("tom_scenario");
    HU_RUN_TEST(tom_scenario_synthesize_false_belief_tag);
    HU_RUN_TEST(tom_gold_matches_response_handles_underscore_tokens);
    HU_RUN_TEST(tom_b8_repo_pack_gold_scores_partial_coverage);
    HU_RUN_TEST(tom_b8_repo_pack_all_items_pass_smoke);
}
