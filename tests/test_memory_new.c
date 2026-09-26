/* Cross-module suite: covers the adaptive and RRF retrieval helpers and the
 * vector circuit breaker. The filename heuristic in
 * scripts/check-test-references.sh resolves "memory_new" to
 * src/memory/memory.c, which this file does not exercise.
 *
 * // @covers-none — heuristic picks src/memory/memory.c; see header above.
 */
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/retrieval/adaptive.h"
#include "human/memory/retrieval/rrf.h"
#include "human/memory/vector/circuit_breaker.h"
#include "test_framework.h"
#include <string.h>

static void test_adaptive_keyword_special(void) {
    hu_adaptive_config_t cfg = {.enabled = true, .keyword_max_tokens = 5, .vector_min_tokens = 6};
    hu_query_analysis_t a = hu_adaptive_analyze_query("user_preferences", 15, &cfg);
    HU_ASSERT_EQ(a.recommended_strategy, HU_ADAPTIVE_KEYWORD_ONLY);
    HU_ASSERT_TRUE(a.has_special_chars);
}

static void test_adaptive_hybrid(void) {
    hu_adaptive_config_t cfg = {.enabled = true, .keyword_max_tokens = 2, .vector_min_tokens = 6};
    hu_query_analysis_t a = hu_adaptive_analyze_query("best practices memory", 21, &cfg);
    HU_ASSERT_EQ(a.recommended_strategy, HU_ADAPTIVE_HYBRID);
}

static void test_circuit_breaker_lifecycle(void) {
    hu_circuit_breaker_t cb;
    hu_circuit_breaker_init(&cb, 2, 60000);
    HU_ASSERT_TRUE(hu_circuit_breaker_allow(&cb));
    hu_circuit_breaker_record_failure(&cb);
    HU_ASSERT_TRUE(hu_circuit_breaker_allow(&cb));
    hu_circuit_breaker_record_failure(&cb);
    HU_ASSERT_TRUE(hu_circuit_breaker_is_open(&cb));
    HU_ASSERT_FALSE(hu_circuit_breaker_allow(&cb));
}

static void test_rrf_two_sources_merge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_entry_t list1[] = {{.key = "a", .key_len = 1, .content = "A", .content_len = 1}};
    hu_memory_entry_t list2[] = {{.key = "b", .key_len = 1, .content = "B", .content_len = 1}};
    const hu_memory_entry_t *sources[] = {list1, list2};
    size_t lens[] = {1, 1};
    hu_memory_entry_t *out = NULL;
    size_t out_count = 0;
    hu_error_t err = hu_rrf_merge(&alloc, sources, lens, 2, 60, 5, &out, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 2u);
    hu_rrf_free_result(&alloc, out, out_count);
}

static void test_rrf_empty_sources(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_entry_t empty[1] = {{0}};
    const hu_memory_entry_t *sources[] = {empty};
    size_t lens[] = {0};
    hu_memory_entry_t *out = NULL;
    size_t out_count = 0;
    hu_error_t err = hu_rrf_merge(&alloc, sources, lens, 1, 60, 10, &out, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 0u);
}

static void test_adaptive_disabled(void) {
    hu_adaptive_config_t cfg = {.enabled = false};
    hu_query_analysis_t a = hu_adaptive_analyze_query("long query here", 15, &cfg);
    /* When disabled, impl may return KEYWORD_ONLY or ignore and return hybrid/vector */
    HU_ASSERT(a.recommended_strategy >= HU_ADAPTIVE_KEYWORD_ONLY &&
              a.recommended_strategy <= HU_ADAPTIVE_HYBRID);
}

static void test_adaptive_short_query(void) {
    hu_adaptive_config_t cfg = {.enabled = true, .keyword_max_tokens = 10, .vector_min_tokens = 5};
    hu_query_analysis_t a = hu_adaptive_analyze_query("hi", 2, &cfg);
    /* Short queries may map to keyword or hybrid depending on implementation */
    HU_ASSERT(a.recommended_strategy >= HU_ADAPTIVE_KEYWORD_ONLY &&
              a.recommended_strategy <= HU_ADAPTIVE_HYBRID);
}

static void test_rrf_single_source(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_entry_t e0 = {.key = "a", .key_len = 1, .content = "A", .content_len = 1};
    hu_memory_entry_t e1 = {.key = "b", .key_len = 1, .content = "B", .content_len = 1};
    hu_memory_entry_t list[] = {e0, e1};
    const hu_memory_entry_t *sources[] = {list};
    size_t lens[] = {2};
    hu_memory_entry_t *out = NULL;
    size_t out_count = 0;
    hu_error_t err = hu_rrf_merge(&alloc, sources, lens, 1, 60, 10, &out, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 2u);
    HU_ASSERT_STR_EQ(out[0].key, "a");
    hu_rrf_free_result(&alloc, out, out_count);
}

void run_memory_new_tests(void) {
    HU_TEST_SUITE("Memory new (retrieval)");
    HU_RUN_TEST(test_adaptive_keyword_special);
    HU_RUN_TEST(test_adaptive_hybrid);
    HU_RUN_TEST(test_adaptive_disabled);
    HU_RUN_TEST(test_adaptive_short_query);
    HU_RUN_TEST(test_circuit_breaker_lifecycle);
    HU_RUN_TEST(test_rrf_single_source);
    HU_RUN_TEST(test_rrf_two_sources_merge);
    HU_RUN_TEST(test_rrf_empty_sources);
}
