#include "human/agent/uncertainty.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

/* AC-4: lock pre-change behavior on the no-real-signals path. This test
 * MUST pass against the unmodified hu_uncertainty_evaluate. After Tasks
 * 2-5 modify the score function, this test still passes — that's the
 * regression contract. */
static void test_score_unchanged_with_no_real_signals(void) {
    hu_allocator_t alloc_storage = hu_system_allocator();
    hu_allocator_t *alloc = &alloc_storage;
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
    HU_ASSERT_EQ(hu_uncertainty_evaluate(alloc, &signals, &result), HU_OK);

    /* Pre-change expected: 0.15 + 0.2 + 0.15 + 0.066 = 0.566 */
    HU_ASSERT_TRUE(result.confidence > 0.565 && result.confidence < 0.567);
    HU_ASSERT_EQ(result.level, HU_CONFIDENCE_MEDIUM);

    hu_uncertainty_result_free(alloc, &result);
}

void run_uncertainty_tests(void) {
    HU_TEST_SUITE("uncertainty");
    HU_RUN_TEST(test_score_unchanged_with_no_real_signals);
}
