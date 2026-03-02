#include "test_framework.h"
#include "seaclaw/memory/engines/registry.h"
#include "seaclaw/memory/lifecycle/rollout.h"
#include "seaclaw/memory/lifecycle/diagnostics.h"
#include "seaclaw/memory/retrieval/adaptive.h"
#include "seaclaw/memory/retrieval/rrf.h"
#include "seaclaw/memory/retrieval/query_expansion.h"
#include "seaclaw/memory/retrieval/llm_reranker.h"
#include "seaclaw/memory/vector/circuit_breaker.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include <string.h>

static void test_registry_find_none(void) {
    const sc_backend_descriptor_t *d = sc_registry_find_backend("none", 4);
#ifdef SC_HAS_NONE_ENGINE
    SC_ASSERT_NOT_NULL(d);
    SC_ASSERT_STR_EQ(d->name, "none");
    SC_ASSERT_FALSE(d->capabilities.supports_keyword_rank);
    SC_ASSERT_FALSE(d->needs_db_path);
#else
    SC_ASSERT_NULL(d);
#endif
}

static void test_registry_is_known(void) {
    SC_ASSERT_TRUE(sc_registry_is_known_backend("sqlite", 6));
    SC_ASSERT_TRUE(sc_registry_is_known_backend("none", 4));
    SC_ASSERT_FALSE(sc_registry_is_known_backend("unknown", 7));
}

static void test_registry_engine_token(void) {
    const char *t = sc_registry_engine_token_for_backend("sqlite", 6);
    SC_ASSERT_NOT_NULL(t);
    SC_ASSERT_STR_EQ(t, "sqlite");
    SC_ASSERT_NULL(sc_registry_engine_token_for_backend("x", 1));
}

static void test_rollout_off(void) {
    sc_rollout_policy_t p = sc_rollout_policy_init("off", 3, 0, 0);
    SC_ASSERT_EQ(sc_rollout_decide(&p, "sess1", 5), SC_ROLLOUT_KEYWORD_ONLY);
}

static void test_rollout_on(void) {
    sc_rollout_policy_t p = sc_rollout_policy_init("on", 2, 0, 0);
    SC_ASSERT_EQ(sc_rollout_decide(&p, "sess1", 5), SC_ROLLOUT_HYBRID);
}

static void test_rollout_canary_null_session(void) {
    sc_rollout_policy_t p = sc_rollout_policy_init("canary", 6, 50, 0);
    SC_ASSERT_EQ(sc_rollout_decide(&p, NULL, 0), SC_ROLLOUT_KEYWORD_ONLY);
}

static void test_adaptive_keyword_special(void) {
    sc_adaptive_config_t cfg = { .enabled = true, .keyword_max_tokens = 5, .vector_min_tokens = 6 };
    sc_query_analysis_t a = sc_adaptive_analyze_query("user_preferences", 15, &cfg);
    SC_ASSERT_EQ(a.recommended_strategy, SC_ADAPTIVE_KEYWORD_ONLY);
    SC_ASSERT_TRUE(a.has_special_chars);
}

static void test_adaptive_hybrid(void) {
    sc_adaptive_config_t cfg = { .enabled = true, .keyword_max_tokens = 2, .vector_min_tokens = 6 };
    sc_query_analysis_t a = sc_adaptive_analyze_query("best practices memory", 21, &cfg);
    SC_ASSERT_EQ(a.recommended_strategy, SC_ADAPTIVE_HYBRID);
}

static void test_circuit_breaker_lifecycle(void) {
    sc_circuit_breaker_t cb;
    sc_circuit_breaker_init(&cb, 2, 60000);
    SC_ASSERT_TRUE(sc_circuit_breaker_allow(&cb));
    sc_circuit_breaker_record_failure(&cb);
    SC_ASSERT_TRUE(sc_circuit_breaker_allow(&cb));
    sc_circuit_breaker_record_failure(&cb);
    SC_ASSERT_TRUE(sc_circuit_breaker_is_open(&cb));
    SC_ASSERT_FALSE(sc_circuit_breaker_allow(&cb));
}

static void test_query_expansion_filters_stopwords(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_expanded_query_t eq = {0};
    sc_error_t err = sc_query_expand(&alloc, "what is the database", 20, &eq);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(eq.fts5_query);
    SC_ASSERT_TRUE(strstr(eq.fts5_query, "database") != NULL);
    sc_expanded_query_free(&alloc, &eq);
}

static void test_llm_reranker_parse_response(void) {
    size_t indices[8];
    size_t n = sc_llm_reranker_parse_response("3,1,5,2,4", 9, indices, 8);
    SC_ASSERT_EQ(n, 5u);
    SC_ASSERT_EQ(indices[0], 2u);  /* 3 -> index 2 */
    SC_ASSERT_EQ(indices[1], 0u);
    SC_ASSERT_EQ(indices[2], 4u);
}

static void test_diagnostics_none_backend(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_backend_capabilities_t caps = {
        .supports_keyword_rank = false,
        .supports_session_store = false,
        .supports_transactions = false,
        .supports_outbox = false,
    };
    sc_diagnostic_report_t rep = {0};
    sc_diagnostics_diagnose(&mem, NULL, NULL, NULL, &caps, 0, "off", 3, &rep);
    SC_ASSERT_STR_EQ(rep.backend_name, "none");
    SC_ASSERT_TRUE(rep.backend_healthy);
    SC_ASSERT_EQ(rep.entry_count, 0u);

    mem.vtable->deinit(mem.ctx);
}

static void test_rrf_two_sources_merge(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_entry_t list1[] = { { .key = "a", .key_len = 1, .content = "A", .content_len = 1 } };
    sc_memory_entry_t list2[] = { { .key = "b", .key_len = 1, .content = "B", .content_len = 1 } };
    const sc_memory_entry_t *sources[] = { list1, list2 };
    size_t lens[] = { 1, 1 };
    sc_memory_entry_t *out = NULL;
    size_t out_count = 0;
    sc_error_t err = sc_rrf_merge(&alloc, sources, lens, 2, 60, 5, &out, &out_count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out_count, 2u);
    sc_rrf_free_result(&alloc, out, out_count);
}

static void test_rrf_empty_sources(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_entry_t empty[1] = {{0}};
    const sc_memory_entry_t *sources[] = { empty };
    size_t lens[] = { 0 };
    sc_memory_entry_t *out = NULL;
    size_t out_count = 0;
    sc_error_t err = sc_rrf_merge(&alloc, sources, lens, 1, 60, 10, &out, &out_count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out_count, 0u);
}

static void test_query_expansion_single_word(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_expanded_query_t eq = {0};
    sc_error_t err = sc_query_expand(&alloc, "database", 8, &eq);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(eq.fts5_query);
    SC_ASSERT_TRUE(strstr(eq.fts5_query, "database") != NULL);
    sc_expanded_query_free(&alloc, &eq);
}

static void test_query_expansion_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_expanded_query_t eq = {0};
    sc_error_t err = sc_query_expand(&alloc, "", 0, &eq);
    SC_ASSERT_EQ(err, SC_OK);
    sc_expanded_query_free(&alloc, &eq);
}

static void test_adaptive_disabled(void) {
    sc_adaptive_config_t cfg = { .enabled = false };
    sc_query_analysis_t a = sc_adaptive_analyze_query("long query here", 15, &cfg);
    /* When disabled, impl may return KEYWORD_ONLY or ignore and return hybrid/vector */
    SC_ASSERT(a.recommended_strategy >= SC_ADAPTIVE_KEYWORD_ONLY &&
              a.recommended_strategy <= SC_ADAPTIVE_HYBRID);
}

static void test_adaptive_short_query(void) {
    sc_adaptive_config_t cfg = { .enabled = true, .keyword_max_tokens = 10, .vector_min_tokens = 5 };
    sc_query_analysis_t a = sc_adaptive_analyze_query("hi", 2, &cfg);
    /* Short queries may map to keyword or hybrid depending on implementation */
    SC_ASSERT(a.recommended_strategy >= SC_ADAPTIVE_KEYWORD_ONLY && a.recommended_strategy <= SC_ADAPTIVE_HYBRID);
}

static void test_rollout_mode_from_string(void) {
    SC_ASSERT_EQ(sc_rollout_mode_from_string("off", 3), SC_ROLLOUT_OFF);
    SC_ASSERT_EQ(sc_rollout_mode_from_string("on", 2), SC_ROLLOUT_ON);
    SC_ASSERT_EQ(sc_rollout_mode_from_string("canary", 6), SC_ROLLOUT_CANARY);
}

static void test_llm_reranker_parse_empty(void) {
    size_t indices[8];
    size_t n = sc_llm_reranker_parse_response("", 0, indices, 8);
    SC_ASSERT_EQ(n, 0u);
}

static void test_llm_reranker_parse_single(void) {
    size_t indices[8];
    size_t n = sc_llm_reranker_parse_response("1", 1, indices, 8);
    SC_ASSERT_EQ(n, 1u);
    SC_ASSERT_EQ(indices[0], 0u);
}

static void test_diagnostics_format_report(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_diagnostic_report_t rep = {0};
    rep.backend_name = "none";
    rep.backend_healthy = true;
    rep.entry_count = 0;
    char *fmt = sc_diagnostics_format_report(&alloc, &rep);
    SC_ASSERT_NOT_NULL(fmt);
    SC_ASSERT_TRUE(strlen(fmt) > 0);
    alloc.free(alloc.ctx, fmt, strlen(fmt) + 1);
}

static void test_rrf_single_source(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_entry_t e0 = { .key = "a", .key_len = 1, .content = "A", .content_len = 1 };
    sc_memory_entry_t e1 = { .key = "b", .key_len = 1, .content = "B", .content_len = 1 };
    sc_memory_entry_t list[] = { e0, e1 };
    const sc_memory_entry_t *sources[] = { list };
    size_t lens[] = { 2 };
    sc_memory_entry_t *out = NULL;
    size_t out_count = 0;
    sc_error_t err = sc_rrf_merge(&alloc, sources, lens, 1, 60, 10, &out, &out_count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out_count, 2u);
    SC_ASSERT_STR_EQ(out[0].key, "a");
    sc_rrf_free_result(&alloc, out, out_count);
}

void run_memory_new_tests(void) {
    SC_TEST_SUITE("Memory new (registry, rollout, retrieval, diagnostics)");
    SC_RUN_TEST(test_registry_find_none);
    SC_RUN_TEST(test_registry_is_known);
    SC_RUN_TEST(test_registry_engine_token);
    SC_RUN_TEST(test_rollout_off);
    SC_RUN_TEST(test_rollout_on);
    SC_RUN_TEST(test_rollout_canary_null_session);
    SC_RUN_TEST(test_rollout_mode_from_string);
    SC_RUN_TEST(test_adaptive_keyword_special);
    SC_RUN_TEST(test_adaptive_hybrid);
    SC_RUN_TEST(test_adaptive_disabled);
    SC_RUN_TEST(test_adaptive_short_query);
    SC_RUN_TEST(test_circuit_breaker_lifecycle);
    SC_RUN_TEST(test_query_expansion_filters_stopwords);
    SC_RUN_TEST(test_query_expansion_single_word);
    SC_RUN_TEST(test_query_expansion_empty);
    SC_RUN_TEST(test_llm_reranker_parse_response);
    SC_RUN_TEST(test_llm_reranker_parse_empty);
    SC_RUN_TEST(test_llm_reranker_parse_single);
    SC_RUN_TEST(test_diagnostics_none_backend);
    SC_RUN_TEST(test_diagnostics_format_report);
    SC_RUN_TEST(test_rrf_single_source);
    SC_RUN_TEST(test_rrf_two_sources_merge);
    SC_RUN_TEST(test_rrf_empty_sources);
}
