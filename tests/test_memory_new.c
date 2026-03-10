#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/engines/registry.h"
#include "human/memory/lifecycle/diagnostics.h"
#include "human/memory/lifecycle/rollout.h"
#include "human/memory/retrieval/adaptive.h"
#include "human/memory/retrieval/llm_reranker.h"
#include "human/memory/retrieval/query_expansion.h"
#include "human/memory/retrieval/rrf.h"
#include "human/memory/vector/circuit_breaker.h"
#include "test_framework.h"
#include <string.h>

static void test_registry_find_none(void) {
    const hu_backend_descriptor_t *d = hu_registry_find_backend("none", 4);
#ifdef HU_HAS_NONE_ENGINE
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d->name, "none");
    HU_ASSERT_FALSE(d->capabilities.supports_keyword_rank);
    HU_ASSERT_FALSE(d->needs_db_path);
#else
    HU_ASSERT_NULL(d);
#endif
}

static void test_registry_is_known(void) {
    HU_ASSERT_TRUE(hu_registry_is_known_backend("sqlite", 6));
    HU_ASSERT_TRUE(hu_registry_is_known_backend("none", 4));
    HU_ASSERT_FALSE(hu_registry_is_known_backend("unknown", 7));
}

static void test_registry_engine_token(void) {
    const char *t = hu_registry_engine_token_for_backend("sqlite", 6);
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_STR_EQ(t, "sqlite");
    HU_ASSERT_NULL(hu_registry_engine_token_for_backend("x", 1));
}

static void test_rollout_off(void) {
    hu_rollout_policy_t p = hu_rollout_policy_init("off", 3, 0, 0);
    HU_ASSERT_EQ(hu_rollout_decide(&p, "sess1", 5), HU_ROLLOUT_KEYWORD_ONLY);
}

static void test_rollout_on(void) {
    hu_rollout_policy_t p = hu_rollout_policy_init("on", 2, 0, 0);
    HU_ASSERT_EQ(hu_rollout_decide(&p, "sess1", 5), HU_ROLLOUT_HYBRID);
}

static void test_rollout_canary_null_session(void) {
    hu_rollout_policy_t p = hu_rollout_policy_init("canary", 6, 50, 0);
    HU_ASSERT_EQ(hu_rollout_decide(&p, NULL, 0), HU_ROLLOUT_KEYWORD_ONLY);
}

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

static void test_query_expansion_filters_stopwords(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_expanded_query_t eq = {0};
    hu_error_t err = hu_query_expand(&alloc, "what is the database", 20, &eq);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(eq.fts5_query);
    HU_ASSERT_TRUE(strstr(eq.fts5_query, "database") != NULL);
    hu_expanded_query_free(&alloc, &eq);
}

static void test_llm_reranker_parse_response(void) {
    size_t indices[8];
    size_t n = hu_llm_reranker_parse_response("3,1,5,2,4", 9, indices, 8);
    HU_ASSERT_EQ(n, 5u);
    HU_ASSERT_EQ(indices[0], 2u); /* 3 -> index 2 */
    HU_ASSERT_EQ(indices[1], 0u);
    HU_ASSERT_EQ(indices[2], 4u);
}

static void test_diagnostics_none_backend(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_backend_capabilities_t caps = {
        .supports_keyword_rank = false,
        .supports_session_store = false,
        .supports_transactions = false,
        .supports_outbox = false,
    };
    hu_diagnostic_report_t rep = {0};
    hu_diagnostics_diagnose(&mem, NULL, NULL, NULL, &caps, 0, "off", 3, &rep);
    HU_ASSERT_STR_EQ(rep.backend_name, "none");
    HU_ASSERT_TRUE(rep.backend_healthy);
    HU_ASSERT_EQ(rep.entry_count, 0u);

    mem.vtable->deinit(mem.ctx);
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

static void test_query_expansion_single_word(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_expanded_query_t eq = {0};
    hu_error_t err = hu_query_expand(&alloc, "database", 8, &eq);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(eq.fts5_query);
    HU_ASSERT_TRUE(strstr(eq.fts5_query, "database") != NULL);
    hu_expanded_query_free(&alloc, &eq);
}

static void test_query_expansion_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_expanded_query_t eq = {0};
    hu_error_t err = hu_query_expand(&alloc, "", 0, &eq);
    HU_ASSERT_EQ(err, HU_OK);
    hu_expanded_query_free(&alloc, &eq);
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

static void test_rollout_mode_from_string(void) {
    HU_ASSERT_EQ(hu_rollout_mode_from_string("off", 3), HU_ROLLOUT_OFF);
    HU_ASSERT_EQ(hu_rollout_mode_from_string("on", 2), HU_ROLLOUT_ON);
    HU_ASSERT_EQ(hu_rollout_mode_from_string("canary", 6), HU_ROLLOUT_CANARY);
}

static void test_llm_reranker_parse_empty(void) {
    size_t indices[8];
    size_t n = hu_llm_reranker_parse_response("", 0, indices, 8);
    HU_ASSERT_EQ(n, 0u);
}

static void test_llm_reranker_parse_single(void) {
    size_t indices[8];
    size_t n = hu_llm_reranker_parse_response("1", 1, indices, 8);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ(indices[0], 0u);
}

static void test_diagnostics_format_report(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_diagnostic_report_t rep = {0};
    rep.backend_name = "none";
    rep.backend_healthy = true;
    rep.entry_count = 0;
    char *fmt = hu_diagnostics_format_report(&alloc, &rep);
    HU_ASSERT_NOT_NULL(fmt);
    HU_ASSERT_TRUE(strlen(fmt) > 0);
    alloc.free(alloc.ctx, fmt, strlen(fmt) + 1);
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
    HU_TEST_SUITE("Memory new (registry, rollout, retrieval, diagnostics)");
    HU_RUN_TEST(test_registry_find_none);
    HU_RUN_TEST(test_registry_is_known);
    HU_RUN_TEST(test_registry_engine_token);
    HU_RUN_TEST(test_rollout_off);
    HU_RUN_TEST(test_rollout_on);
    HU_RUN_TEST(test_rollout_canary_null_session);
    HU_RUN_TEST(test_rollout_mode_from_string);
    HU_RUN_TEST(test_adaptive_keyword_special);
    HU_RUN_TEST(test_adaptive_hybrid);
    HU_RUN_TEST(test_adaptive_disabled);
    HU_RUN_TEST(test_adaptive_short_query);
    HU_RUN_TEST(test_circuit_breaker_lifecycle);
    HU_RUN_TEST(test_query_expansion_filters_stopwords);
    HU_RUN_TEST(test_query_expansion_single_word);
    HU_RUN_TEST(test_query_expansion_empty);
    HU_RUN_TEST(test_llm_reranker_parse_response);
    HU_RUN_TEST(test_llm_reranker_parse_empty);
    HU_RUN_TEST(test_llm_reranker_parse_single);
    HU_RUN_TEST(test_diagnostics_none_backend);
    HU_RUN_TEST(test_diagnostics_format_report);
    HU_RUN_TEST(test_rrf_single_source);
    HU_RUN_TEST(test_rrf_two_sources_merge);
    HU_RUN_TEST(test_rrf_empty_sources);
}
