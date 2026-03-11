#include "human/agent/arbitrator.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void arbitrator_estimate_tokens_simple(void) {
    HU_ASSERT_EQ(hu_directive_estimate_tokens("hello", 5), 2u);
}

static void arbitrator_estimate_tokens_empty(void) {
    HU_ASSERT_EQ(hu_directive_estimate_tokens("", 0), 1u);
}

static void arbitrator_compute_priority_all_max(void) {
    double p = hu_directive_compute_priority(0, 1.0, 1.0, 1.0, 1.0, 1.0);
    HU_ASSERT_FLOAT_EQ(p, 1.0, 0.01);
}

static void arbitrator_compute_priority_all_zero(void) {
    double p = hu_directive_compute_priority(0, 0.0, 0.0, 0.0, 0.0, 0.0);
    HU_ASSERT_FLOAT_EQ(p, 0.0, 0.01);
}

static void arbitrator_compute_priority_recency_dominates(void) {
    double p = hu_directive_compute_priority(0, 1.0, 0.0, 0.0, 0.0, 0.0);
    HU_ASSERT_FLOAT_EQ(p, 0.30, 0.01);
}

static void arbitrator_resolve_conflicts_removes_loser(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_directive_t d[2];
    memset(d, 0, sizeof(d));
    d[0].content = hu_strndup(&alloc, "a", 1);
    d[0].content_len = 1;
    d[0].source = hu_strndup(&alloc, "energy_match", 12);
    d[0].source_len = 12;
    d[0].priority = 0.5;
    d[1].content = hu_strndup(&alloc, "b", 1);
    d[1].content_len = 1;
    d[1].source = hu_strndup(&alloc, "emotional_checkin", 17);
    d[1].source_len = 17;
    d[1].priority = 0.3;

    hu_directive_conflict_t rule = {"energy_match", "emotional_checkin", "energy_match"};
    size_t n = hu_arbitrator_resolve_conflicts(&alloc, d, 2, &rule, 1);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_STR_EQ(d[0].source, "energy_match");

    hu_directive_deinit(&alloc, &d[0]);
}

static void arbitrator_resolve_conflicts_keeps_higher_priority_when_no_winner(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_directive_t d[2];
    memset(d, 0, sizeof(d));
    d[0].content = hu_strndup(&alloc, "low", 3);
    d[0].content_len = 3;
    d[0].source = hu_strndup(&alloc, "src_a", 5);
    d[0].source_len = 5;
    d[0].priority = 0.2;
    d[1].content = hu_strndup(&alloc, "high", 4);
    d[1].content_len = 4;
    d[1].source = hu_strndup(&alloc, "src_b", 5);
    d[1].source_len = 5;
    d[1].priority = 0.9;

    hu_directive_conflict_t rule = {"src_a", "src_b", NULL};
    size_t n = hu_arbitrator_resolve_conflicts(&alloc, d, 2, &rule, 1);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_STR_EQ(d[0].source, "src_b");

    hu_directive_deinit(&alloc, &d[0]);
}

static void arbitrator_resolve_conflicts_no_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_directive_t d[2];
    memset(d, 0, sizeof(d));
    d[0].source = hu_strndup(&alloc, "foo", 3);
    d[0].source_len = 3;
    d[1].source = hu_strndup(&alloc, "bar", 3);
    d[1].source_len = 3;

    hu_directive_conflict_t rule = {"energy_match", "emotional_checkin", "energy_match"};
    size_t n = hu_arbitrator_resolve_conflicts(&alloc, d, 2, &rule, 1);
    HU_ASSERT_EQ(n, 2u);

    hu_directive_deinit(&alloc, &d[0]);
    hu_directive_deinit(&alloc, &d[1]);
}

static void arbitrator_select_includes_all_required(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_directive_t candidates[5];
    memset(candidates, 0, sizeof(candidates));
    candidates[0].content = hu_strndup(&alloc, "req1", 4);
    candidates[0].content_len = 4;
    candidates[0].source = hu_strndup(&alloc, "s1", 2);
    candidates[0].source_len = 2;
    candidates[0].priority = 0.5;
    candidates[0].token_cost = 2;
    candidates[0].required = true;
    candidates[1].content = hu_strndup(&alloc, "req2", 4);
    candidates[1].content_len = 4;
    candidates[1].source = hu_strndup(&alloc, "s2", 2);
    candidates[1].source_len = 2;
    candidates[1].priority = 0.5;
    candidates[1].token_cost = 2;
    candidates[1].required = true;
    candidates[2].content = hu_strndup(&alloc, "opt1", 4);
    candidates[2].content_len = 4;
    candidates[2].source = hu_strndup(&alloc, "s3", 2);
    candidates[2].source_len = 2;
    candidates[2].priority = 0.9;
    candidates[2].token_cost = 2;
    candidates[2].required = false;
    candidates[3].content = hu_strndup(&alloc, "opt2", 4);
    candidates[3].content_len = 4;
    candidates[3].source = hu_strndup(&alloc, "s4", 2);
    candidates[3].source_len = 2;
    candidates[3].priority = 0.1;
    candidates[3].token_cost = 2;
    candidates[3].required = false;
    candidates[4].content = hu_strndup(&alloc, "opt3", 4);
    candidates[4].content_len = 4;
    candidates[4].source = hu_strndup(&alloc, "s5", 2);
    candidates[4].source_len = 2;
    candidates[4].priority = 0.2;
    candidates[4].token_cost = 2;
    candidates[4].required = false;

    hu_arbitration_config_t config = {.max_directive_tokens = 10, .max_directives = 3};
    hu_arbitration_result_t result;
    HU_ASSERT_EQ(hu_arbitrator_select(&alloc, candidates, 5, &config, &result), HU_OK);
    HU_ASSERT_EQ(result.selected_count, 3u);
    HU_ASSERT_EQ(result.total_tokens, 6u);
    HU_ASSERT_EQ(result.suppressed_count, 2u);
    /* Both required + 1 optional (highest priority) */
    bool has_req1 = false, has_req2 = false, has_opt1 = false;
    for (size_t i = 0; i < result.selected_count; i++) {
        if (result.selected[i].required) {
            if (strcmp(result.selected[i].source, "s1") == 0)
                has_req1 = true;
            if (strcmp(result.selected[i].source, "s2") == 0)
                has_req2 = true;
        } else if (strcmp(result.selected[i].source, "s3") == 0)
            has_opt1 = true;
    }
    HU_ASSERT_TRUE(has_req1);
    HU_ASSERT_TRUE(has_req2);
    HU_ASSERT_TRUE(has_opt1);

    hu_arbitration_result_deinit(&alloc, &result);
    for (int i = 0; i < 5; i++)
        hu_directive_deinit(&alloc, &candidates[i]);
}

static void arbitrator_select_respects_token_budget(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_directive_t candidates[5];
    memset(candidates, 0, sizeof(candidates));
    for (int i = 0; i < 5; i++) {
        candidates[i].content = hu_strndup(&alloc, "x", 1);
        candidates[i].content_len = 1;
        candidates[i].source = hu_strndup(&alloc, "s", 1);
        candidates[i].source_len = 1;
        candidates[i].priority = 0.5;
        candidates[i].token_cost = 500;
        candidates[i].required = false;
    }

    hu_arbitration_config_t config = {.max_directive_tokens = 1500, .max_directives = 10};
    hu_arbitration_result_t result;
    HU_ASSERT_EQ(hu_arbitrator_select(&alloc, candidates, 5, &config, &result), HU_OK);
    HU_ASSERT_EQ(result.selected_count, 3u);
    HU_ASSERT_TRUE(result.total_tokens <= 1500u);

    hu_arbitration_result_deinit(&alloc, &result);
    for (int i = 0; i < 5; i++)
        hu_directive_deinit(&alloc, &candidates[i]);
}

static void arbitrator_select_respects_max_directives(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_directive_t candidates[10];
    memset(candidates, 0, sizeof(candidates));
    for (int i = 0; i < 10; i++) {
        char buf[16];
        int n = (int)snprintf(buf, sizeof(buf), "s%d", i);
        candidates[i].content = hu_strndup(&alloc, "x", 1);
        candidates[i].content_len = 1;
        candidates[i].source = hu_strndup(&alloc, buf, (size_t)n);
        candidates[i].source_len = (size_t)n;
        candidates[i].priority = 1.0;
        candidates[i].token_cost = 1;
        candidates[i].required = false;
    }

    hu_arbitration_config_t config = {.max_directive_tokens = 10000, .max_directives = 4};
    hu_arbitration_result_t result;
    HU_ASSERT_EQ(hu_arbitrator_select(&alloc, candidates, 10, &config, &result), HU_OK);
    HU_ASSERT_EQ(result.selected_count, 4u);

    hu_arbitration_result_deinit(&alloc, &result);
    for (int i = 0; i < 10; i++)
        hu_directive_deinit(&alloc, &candidates[i]);
}

static void arbitrator_select_prefers_high_priority(void) {
    hu_allocator_t alloc = hu_system_allocator();
    double priorities[] = {0.9, 0.1, 0.5, 0.8, 0.3};
    hu_directive_t candidates[5];
    memset(candidates, 0, sizeof(candidates));
    for (int i = 0; i < 5; i++) {
        char buf[16];
        int n = (int)snprintf(buf, sizeof(buf), "s%d", i);
        candidates[i].content = hu_strndup(&alloc, "x", 1);
        candidates[i].content_len = 1;
        candidates[i].source = hu_strndup(&alloc, buf, (size_t)n);
        candidates[i].source_len = (size_t)n;
        candidates[i].priority = priorities[i];
        candidates[i].token_cost = 10;
        candidates[i].required = false;
    }

    hu_arbitration_config_t config = {.max_directive_tokens = 50, .max_directives = 3};
    hu_arbitration_result_t result;
    HU_ASSERT_EQ(hu_arbitrator_select(&alloc, candidates, 5, &config, &result), HU_OK);
    HU_ASSERT_EQ(result.selected_count, 3u);
    /* Top 3 by priority: 0.9, 0.8, 0.5 */
    HU_ASSERT_FLOAT_EQ(result.selected[0].priority, 0.9, 0.01);
    HU_ASSERT_FLOAT_EQ(result.selected[1].priority, 0.8, 0.01);
    HU_ASSERT_FLOAT_EQ(result.selected[2].priority, 0.5, 0.01);

    hu_arbitration_result_deinit(&alloc, &result);
    for (int i = 0; i < 5; i++)
        hu_directive_deinit(&alloc, &candidates[i]);
}

static void arbitrator_select_null_config_uses_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_directive_t candidates[1];
    memset(candidates, 0, sizeof(candidates));
    candidates[0].content = hu_strndup(&alloc, "x", 1);
    candidates[0].content_len = 1;
    candidates[0].source = hu_strndup(&alloc, "s", 1);
    candidates[0].source_len = 1;
    candidates[0].priority = 1.0;
    candidates[0].token_cost = 100;
    candidates[0].required = false;

    hu_arbitration_result_t result;
    HU_ASSERT_EQ(hu_arbitrator_select(&alloc, candidates, 1, NULL, &result), HU_OK);
    HU_ASSERT_EQ(result.selected_count, 1u);
    HU_ASSERT_TRUE(result.total_tokens <= 1500u);

    hu_arbitration_result_deinit(&alloc, &result);
    hu_directive_deinit(&alloc, &candidates[0]);
}

static void arbitrator_select_empty_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_arbitration_result_t result;
    const hu_directive_t *empty = NULL;
    HU_ASSERT_EQ(hu_arbitrator_select(&alloc, empty, 0, NULL, &result), HU_OK);
    HU_ASSERT_EQ(result.selected_count, 0u);
    HU_ASSERT_NULL(result.selected);
}

static void arbitrator_result_deinit_frees_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_directive_t candidates[1];
    memset(candidates, 0, sizeof(candidates));
    candidates[0].content = hu_strndup(&alloc, "content", 7);
    candidates[0].content_len = 7;
    candidates[0].source = hu_strndup(&alloc, "source", 6);
    candidates[0].source_len = 6;
    candidates[0].priority = 1.0;
    candidates[0].token_cost = 2;
    candidates[0].required = false;

    hu_arbitration_result_t result;
    HU_ASSERT_EQ(hu_arbitrator_select(&alloc, candidates, 1, NULL, &result), HU_OK);
    HU_ASSERT_EQ(result.selected_count, 1u);
    hu_arbitration_result_deinit(&alloc, &result);
    HU_ASSERT_NULL(result.selected);
    HU_ASSERT_EQ(result.selected_count, 0u);

    hu_directive_deinit(&alloc, &candidates[0]);
}

static void arbitrator_select_required_exceeds_budget_still_included(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_directive_t candidates[1];
    memset(candidates, 0, sizeof(candidates));
    candidates[0].content = hu_strndup(&alloc, "huge", 4);
    candidates[0].content_len = 4;
    candidates[0].source = hu_strndup(&alloc, "safety", 6);
    candidates[0].source_len = 6;
    candidates[0].priority = 0.1;
    candidates[0].token_cost = 10000;
    candidates[0].required = true;

    hu_arbitration_config_t config = {.max_directive_tokens = 100, .max_directives = 7};
    hu_arbitration_result_t result;
    HU_ASSERT_EQ(hu_arbitrator_select(&alloc, candidates, 1, &config, &result), HU_OK);
    HU_ASSERT_EQ(result.selected_count, 1u);
    HU_ASSERT_TRUE(result.selected[0].required);
    HU_ASSERT_EQ(result.total_tokens, 10000u);

    hu_arbitration_result_deinit(&alloc, &result);
    hu_directive_deinit(&alloc, &candidates[0]);
}

void run_arbitrator_tests(void) {
    HU_TEST_SUITE("arbitrator");
    HU_RUN_TEST(arbitrator_estimate_tokens_simple);
    HU_RUN_TEST(arbitrator_estimate_tokens_empty);
    HU_RUN_TEST(arbitrator_compute_priority_all_max);
    HU_RUN_TEST(arbitrator_compute_priority_all_zero);
    HU_RUN_TEST(arbitrator_compute_priority_recency_dominates);
    HU_RUN_TEST(arbitrator_resolve_conflicts_removes_loser);
    HU_RUN_TEST(arbitrator_resolve_conflicts_keeps_higher_priority_when_no_winner);
    HU_RUN_TEST(arbitrator_resolve_conflicts_no_match);
    HU_RUN_TEST(arbitrator_select_includes_all_required);
    HU_RUN_TEST(arbitrator_select_respects_token_budget);
    HU_RUN_TEST(arbitrator_select_respects_max_directives);
    HU_RUN_TEST(arbitrator_select_prefers_high_priority);
    HU_RUN_TEST(arbitrator_select_null_config_uses_defaults);
    HU_RUN_TEST(arbitrator_select_empty_input);
    HU_RUN_TEST(arbitrator_result_deinit_frees_memory);
    HU_RUN_TEST(arbitrator_select_required_exceeds_budget_still_included);
}
