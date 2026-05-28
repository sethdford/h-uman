#include "human/agent/uncertainty.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

/* AC-4: lock pre-change behavior on the no-real-signals path. This test
 * MUST pass against the unmodified hu_uncertainty_evaluate. After Tasks
 * 2-5 modify the score function, this test still passes — that's the
 * regression contract. */
static void test_score_unchanged_with_no_real_signals(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_signals_t signals = {0};
    signals.retrieval_coverage = 0.5; /* contributes 0.15 */
    signals.tool_results_count = 1;   /* contributes 0.2 */
    signals.has_citations = false;
    signals.has_hedging_language = false; /* confident language → 0.15 */
    signals.memory_results_count = 2;     /* 2 * 0.033 = 0.066 */
    signals.is_factual_query = true;      /* no opinion bonus */
    /* NEW fields explicitly zero — exercises the no-real-signals path */
    signals.grounded_confidence = 0.0;
    signals.fact_count = 0;
    signals.verbalized_confidence = 0.0;
    signals.has_verbalized = false;
    signals.contradiction_present = false;
    signals.has_temporal_decay = false;

    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);

    /* Pre-change expected: 0.15 + 0.2 + 0.15 + 0.066 = 0.566 */
    HU_ASSERT_TRUE(result.confidence > 0.565 && result.confidence < 0.567);
    HU_ASSERT_EQ(result.level, HU_CONFIDENCE_MEDIUM);

    hu_uncertainty_result_free(&alloc, &result);
}

static void test_score_blend_at_one_fact(void) {
    /* fact_count=1, grounded_confidence=0.9 → 33% real + 67% heuristic
     * Set only retrieval_coverage=0.5 to get heuristic=0.15, avoiding
     * default-signal contributions (e.g. !has_hedging_language → +0.15) */
    hu_uncertainty_signals_t signals = {0};
    signals.retrieval_coverage = 0.5;
    signals.fact_count = 1;
    signals.grounded_confidence = 0.9;
    /* Explicitly set these to prevent default-false signals from boosting score */
    signals.has_citations = false;
    signals.has_hedging_language = true; /* suppress the !hedging bonus */
    signals.is_factual_query = true;     /* suppress the !factual bonus */
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    /* Expect: (1-1/3)*0.15 + (1/3)*0.9 = 0.1 + 0.3 = 0.4 */
    HU_ASSERT_TRUE(result.confidence > 0.37 && result.confidence < 0.43);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_score_blend_at_three_facts(void) {
    /* fact_count=3 → 100% real signal. heuristics contribute 0 */
    hu_uncertainty_signals_t signals = {0};
    signals.retrieval_coverage = 0.5;
    signals.fact_count = 3;
    signals.grounded_confidence = 0.85;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.84 && result.confidence < 0.86);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_grounded_confidence_uses_effective_decay(void) {
    /* 60-day-old 0.9 fact arrives here as 0.57 (decay already applied
       at agent_turn integration). Pure consumption test. */
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.57;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    HU_ASSERT_EQ(result.level, HU_CONFIDENCE_MEDIUM);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_contradiction_penalty_applies(void) {
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.85;
    signals.contradiction_present = true;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    /* 0.85 - 0.15 = 0.70 */
    HU_ASSERT_TRUE(result.confidence > 0.69 && result.confidence < 0.71);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_verbalized_low_pulls_score_down(void) {
    /* Model self-reports 0.3 vs blended 0.7 → result = 0.6*0.7 + 0.4*0.3 = 0.54 */
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.7;
    signals.has_verbalized = true;
    signals.verbalized_confidence = 0.3;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.53 && result.confidence < 0.55);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_verbalized_high_does_not_over_inflate(void) {
    /* Model claims 0.95, signals say 0.6 → stays near 0.6 (asymmetric rule) */
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.6;
    signals.has_verbalized = true;
    signals.verbalized_confidence = 0.95;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.58 && result.confidence < 0.62);
    hu_uncertainty_result_free(&alloc, &result);
}

/* Task 3: Strip and parse verbalized confidence tags */
static void test_strip_verbalized_tag_at_response_tail(void) {
    char response[] = "She said Thursday. [conf=0.7]";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(parsed_conf > 0.69 && parsed_conf < 0.71);
    response[len] = '\0';
    HU_ASSERT_STR_EQ(response, "She said Thursday.");
}

static void test_strip_verbalized_no_tag_returns_no_match(void) {
    char response[] = "Plain answer with no tag.";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_FALSE(found);
    HU_ASSERT_EQ(len, strlen("Plain answer with no tag."));
}

void run_uncertainty_tests(void) {
    HU_TEST_SUITE("uncertainty");
    HU_RUN_TEST(test_score_unchanged_with_no_real_signals);
    HU_RUN_TEST(test_score_blend_at_one_fact);
    HU_RUN_TEST(test_score_blend_at_three_facts);
    HU_RUN_TEST(test_grounded_confidence_uses_effective_decay);
    HU_RUN_TEST(test_contradiction_penalty_applies);
    HU_RUN_TEST(test_verbalized_low_pulls_score_down);
    HU_RUN_TEST(test_verbalized_high_does_not_over_inflate);
    HU_RUN_TEST(test_strip_verbalized_tag_at_response_tail);
    HU_RUN_TEST(test_strip_verbalized_no_tag_returns_no_match);
}
