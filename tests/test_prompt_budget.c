/* Pure-predicate tests for hu_prompt_budget_t. No live prompt build path
 * — those tests come in a Task-1b slice when the wrapping in prompt.c
 * lands. This file pins the OBSERVER + DECISION logic in isolation. */

#include "human/agent/prompt_budget.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static void test_field_name_returns_stable_string(void) {
    /* hu_prompt_field_name must return a non-null static string for
     * every valid index — the budget's snapshot relies on this. */
    HU_ASSERT_STR_EQ(hu_prompt_field_name(HU_PROMPT_FIELD_MEMORY_CONTEXT), "memory_context");
    HU_ASSERT_STR_EQ(hu_prompt_field_name(HU_PROMPT_FIELD_CONVERSATION_CONTEXT),
                     "conversation_context");
    HU_ASSERT_STR_EQ(hu_prompt_field_name(HU_PROMPT_FIELD_SOMATIC_CONTEXT), "somatic_context");
    /* Out-of-range returns NULL (defensive — no array-bounds read). */
    HU_ASSERT(hu_prompt_field_name((hu_prompt_field_t)HU_PROMPT_FIELD_COUNT) == NULL);
    HU_ASSERT(hu_prompt_field_name((hu_prompt_field_t)-1) == NULL);
}

static void test_init_free_round_trip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    HU_ASSERT_NOT_NULL(b);
    HU_ASSERT_EQ(hu_prompt_budget_observation_count(b), (size_t)0);
    hu_prompt_budget_free(b);
}

static void test_init_null_args_return_invalid(void) {
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(NULL, &b), HU_ERR_INVALID_ARGUMENT);
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, NULL), HU_ERR_INVALID_ARGUMENT);
    /* hu_prompt_budget_free(NULL) must be safe. */
    hu_prompt_budget_free(NULL);
}

static void test_observe_single_turn_advances_counter(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].bytes_contributed = 1024;
    stats[HU_PROMPT_FIELD_CONVERSATION_CONTEXT].bytes_contributed = 512;
    hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT_EQ(hu_prompt_budget_observation_count(b), (size_t)1);
    hu_prompt_budget_free(b);
}

static void test_dead_field_requires_min_samples(void) {
    /* A field cannot be tagged DEAD before we have enough observations,
     * even if every observed sample is zero. Prevents trim-on-first-tick. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    /* 5 observations, all zero bytes for somatic. */
    for (int i = 0; i < 5; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    /* Threshold is min_samples=100; 5 < 100 → still UNKNOWN, not DEAD. */
    HU_ASSERT(!hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_SOMATIC_CONTEXT, 16, 100));
    hu_prompt_budget_free(b);
}

static void test_dead_field_fires_after_threshold_with_zero_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    /* 100 observations, all zero — somatic IS dead. */
    for (int i = 0; i < 100; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT(hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_SOMATIC_CONTEXT, 16, 100));
}

static void test_dead_field_skips_when_field_is_populated(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    /* 100 observations, conversation field has 2000 bytes each — clearly
     * NOT dead. Mean 2000 > threshold 16. */
    stats[HU_PROMPT_FIELD_CONVERSATION_CONTEXT].bytes_contributed = 2000;
    for (int i = 0; i < 100; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT(!hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_CONVERSATION_CONTEXT, 16, 100));
    /* Memory field IS dead (mean 0). */
    HU_ASSERT(hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_MEMORY_CONTEXT, 16, 100));
    hu_prompt_budget_free(b);
}

static void test_dead_field_borderline_mean_below_threshold(void) {
    /* A field that consistently produces a small but non-zero contribution
     * (e.g. always exactly 8 bytes of header) should still tag as DEAD
     * when the threshold is 16. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    stats[HU_PROMPT_FIELD_RUPTURE_CONTEXT].bytes_contributed = 8;
    for (int i = 0; i < 100; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT(hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_RUPTURE_CONTEXT, 16, 100));
    /* Same field above threshold = not DEAD. */
    HU_ASSERT(!hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_RUPTURE_CONTEXT, 4, 100));
    hu_prompt_budget_free(b);
}

static void test_dead_field_with_one_outlier_observation(void) {
    /* If a field is non-zero in even ONE turn out of many, the mean
     * may still fall below threshold but the field arguably isn't
     * "dead." This test pins the current behavior (mean-based) so a
     * future change to "non_empty_count"-based logic is intentional. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    /* 99 zero observations + 1 with 100 bytes → mean = 1 byte < 16 → DEAD. */
    for (int i = 0; i < 99; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    stats[HU_PROMPT_FIELD_MOMENT_CONTEXT].bytes_contributed = 100;
    hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT(hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_MOMENT_CONTEXT, 16, 100));
    hu_prompt_budget_free(b);
}

static void test_snapshot_reports_mean_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].bytes_contributed = 1000;
    stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].name =
        hu_prompt_field_name(HU_PROMPT_FIELD_MEMORY_CONTEXT);
    for (int i = 0; i < 4; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    hu_prompt_field_stat_t snap[HU_PROMPT_FIELD_COUNT];
    size_t n = hu_prompt_budget_snapshot(b, snap, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT_EQ(n, (size_t)HU_PROMPT_FIELD_COUNT);
    HU_ASSERT_EQ(snap[HU_PROMPT_FIELD_MEMORY_CONTEXT].bytes_contributed, (size_t)1000);
    HU_ASSERT_STR_EQ(snap[HU_PROMPT_FIELD_MEMORY_CONTEXT].name, "memory_context");
    /* Unobserved field reports zero mean but still has a name. */
    HU_ASSERT_EQ(snap[HU_PROMPT_FIELD_CONVERSATION_CONTEXT].bytes_contributed, (size_t)0);
    HU_ASSERT_STR_EQ(snap[HU_PROMPT_FIELD_CONVERSATION_CONTEXT].name, "conversation_context");
    hu_prompt_budget_free(b);
}

static void test_snapshot_respects_array_cap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t small[3];
    memset(small, 0, sizeof(small));
    size_t n = hu_prompt_budget_snapshot(b, small, 3);
    HU_ASSERT_EQ(n, (size_t)3);
    hu_prompt_budget_free(b);
}

static void test_observe_null_args_no_op(void) {
    /* All NULL combinations are no-ops (no crash, no state mutation). */
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    hu_prompt_budget_observe(NULL, stats, HU_PROMPT_FIELD_COUNT);

    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_budget_observe(b, NULL, HU_PROMPT_FIELD_COUNT);
    hu_prompt_budget_observe(b, stats, 0);
    HU_ASSERT_EQ(hu_prompt_budget_observation_count(b), (size_t)0);
    hu_prompt_budget_free(b);
}

static void test_field_is_dead_null_budget_returns_false(void) {
    /* Defensive: NULL budget never reports a field as dead — caller
     * should never act on uninitialized telemetry. */
    HU_ASSERT(!hu_prompt_budget_field_is_dead(NULL, HU_PROMPT_FIELD_MEMORY_CONTEXT, 16, 100));
}

void run_prompt_budget_tests(void);
void run_prompt_budget_tests(void) {
    HU_TEST_SUITE("prompt_budget");
    HU_RUN_TEST(test_field_name_returns_stable_string);
    HU_RUN_TEST(test_init_free_round_trip);
    HU_RUN_TEST(test_init_null_args_return_invalid);
    HU_RUN_TEST(test_observe_single_turn_advances_counter);
    HU_RUN_TEST(test_dead_field_requires_min_samples);
    HU_RUN_TEST(test_dead_field_fires_after_threshold_with_zero_bytes);
    HU_RUN_TEST(test_dead_field_skips_when_field_is_populated);
    HU_RUN_TEST(test_dead_field_borderline_mean_below_threshold);
    HU_RUN_TEST(test_dead_field_with_one_outlier_observation);
    HU_RUN_TEST(test_snapshot_reports_mean_bytes);
    HU_RUN_TEST(test_snapshot_respects_array_cap);
    HU_RUN_TEST(test_observe_null_args_no_op);
    HU_RUN_TEST(test_field_is_dead_null_budget_returns_false);
}
