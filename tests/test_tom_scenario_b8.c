#include "human/agent.h"
#include "human/agent/tom_scenario.h"
#include "human/core/allocator.h"
#include "test_framework.h"

#include <stdlib.h>
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
    HU_ASSERT_GE(pass, 3u);
}

static void tom_b8_score_responses_pass_when_response_contains_gold(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[768];
    int n = snprintf(path, sizeof(path), "%s/tom/tom_synthetic.json", HU_EVAL_SUITES_DIR);
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(path));

    static const char r1[] = "Max will look in the original basket first.";
    static const char r2[] = "Sam still believes the cookies are in the jar.";
    hu_tom_b8_response_t responses[] = {
        {.id = "tom-fb-01", .response = r1, .response_len = sizeof(r1) - 1},
        {.id = "tom-fb-02", .response = r2, .response_len = sizeof(r2) - 1},
    };
    unsigned pass = 0, total = 0;
    HU_ASSERT_EQ(hu_tom_b8_synthetic_pack_score_responses(&alloc, path, responses, 2, 0, &pass,
                                                          &total),
                 HU_OK);
    HU_ASSERT_EQ(total, 2u);
    HU_ASSERT_EQ(pass, 2u);
}

static void tom_b8_score_responses_unanswered_policy_controls_total(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[768];
    int n = snprintf(path, sizeof(path), "%s/tom/tom_synthetic.json", HU_EVAL_SUITES_DIR);
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(path));

    unsigned pass = 0, total = 0;
    HU_ASSERT_EQ(hu_tom_b8_synthetic_pack_score_responses(&alloc, path, NULL, 0, 0, &pass, &total),
                 HU_OK);
    HU_ASSERT_EQ(pass, 0u);
    HU_ASSERT_EQ(total, 0u);

    HU_ASSERT_EQ(hu_tom_b8_synthetic_pack_score_responses(&alloc, path, NULL, 0, 1, &pass, &total),
                 HU_OK);
    HU_ASSERT_EQ(pass, 0u);
    HU_ASSERT_EQ(total, 10u);
}

static void tom_b8_score_responses_rejects_invalid_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    unsigned pass = 0, total = 0;
    HU_ASSERT_EQ(hu_tom_b8_synthetic_pack_score_responses(NULL, "x", NULL, 0, 0, &pass, &total),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_tom_b8_synthetic_pack_score_responses(&alloc, NULL, NULL, 0, 0, &pass, &total),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_tom_b8_synthetic_pack_score_responses(&alloc, "x", NULL, 1, 0, &pass, &total),
                 HU_ERR_INVALID_ARGUMENT);
}

static void tom_agent_set_scenario_truncates_and_clears(void) {
    hu_agent_t *agent = (hu_agent_t *)calloc(1, sizeof(hu_agent_t));
    HU_ASSERT_NOT_NULL(agent);

    hu_agent_set_tom_scenario(agent, "premise text", "question text", "false_belief");
    HU_ASSERT_EQ(strcmp(agent->tom_scenario_premise, "premise text"), 0);
    HU_ASSERT_EQ(strcmp(agent->tom_scenario_question, "question text"), 0);
    HU_ASSERT_EQ(strcmp(agent->tom_scenario_category, "false_belief"), 0);

    hu_agent_set_tom_scenario(agent, NULL, "", NULL);
    HU_ASSERT_EQ(agent->tom_scenario_premise[0], '\0');
    HU_ASSERT_EQ(agent->tom_scenario_question[0], '\0');
    HU_ASSERT_EQ(agent->tom_scenario_category[0], '\0');

    char big[2048];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    hu_agent_set_tom_scenario(agent, big, big, big);
    HU_ASSERT_EQ(strlen(agent->tom_scenario_premise), sizeof(agent->tom_scenario_premise) - 1);
    HU_ASSERT_EQ(strlen(agent->tom_scenario_question), sizeof(agent->tom_scenario_question) - 1);
    HU_ASSERT_EQ(strlen(agent->tom_scenario_category), sizeof(agent->tom_scenario_category) - 1);

    hu_agent_set_tom_scenario(NULL, "", "", "");

    free(agent);
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
    HU_RUN_TEST(tom_b8_score_responses_pass_when_response_contains_gold);
    HU_RUN_TEST(tom_b8_score_responses_unanswered_policy_controls_total);
    HU_RUN_TEST(tom_b8_score_responses_rejects_invalid_args);
    HU_RUN_TEST(tom_agent_set_scenario_truncates_and_clears);
    HU_RUN_TEST(tom_b8_repo_pack_all_items_pass_smoke);
}
