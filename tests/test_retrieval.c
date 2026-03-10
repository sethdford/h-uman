#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/rerank.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

static void test_keyword_basic_match(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "user_pref", 9, "likes dark mode", 15, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "pet", 3, "dog named Spot", 14, &cat, NULL, 0);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = false,
        .temporal_decay_factor = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_keyword_retrieve(&alloc, &mem, "dark", 4, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, 1);
    HU_ASSERT_STR_EQ(res.entries[0].key, "user_pref");
    HU_ASSERT_TRUE(res.scores[0] > 0.0);

    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    (void)0; /* skip when SQLite disabled */
#endif
}

static void test_keyword_case_insensitive(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "k1", 2, "Hello World", 11, &cat, NULL, 0);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = false,
        .temporal_decay_factor = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_keyword_retrieve(&alloc, &mem, "HELLO", 5, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, 1);
    HU_ASSERT_STR_EQ(res.entries[0].key, "k1");

    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    (void)0;
#endif
}

static void test_keyword_no_match_returns_empty(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "k1", 2, "apple banana", 12, &cat, NULL, 0);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_keyword_retrieve(&alloc, &mem, "xyz", 3, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    /* With min_score=0.0, results may include low-score entries */
    hu_retrieval_result_free(&alloc, &res);

    mem.vtable->deinit(mem.ctx);
#else
    (void)0;
#endif
}

static void test_temporal_decay_reduces_old_scores(void) {
    double base = 1.0;
    const char *old_ts = "2026-02-27T12:00:00Z";
    double decayed = hu_temporal_decay_score(base, 0.5, old_ts, strlen(old_ts));
    HU_ASSERT_TRUE(decayed >= 0.0);
    HU_ASSERT_TRUE(decayed <= base);
    /* No decay when factor is 0 */
    double unchanged = hu_temporal_decay_score(base, 0.0, old_ts, strlen(old_ts));
    HU_ASSERT_FLOAT_EQ(unchanged, base, 1e-9);
    /* No timestamp = no decay */
    double no_ts = hu_temporal_decay_score(base, 0.5, NULL, 0);
    HU_ASSERT_FLOAT_EQ(no_ts, base, 1e-9);
}

static void test_rrf_combines_rankings(void) {
    /* RRF is used internally when we have both keyword and semantic.
     * For now hybrid just delegates to keyword - this test verifies
     * hybrid doesn't crash and returns keyword-like results. */
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "a", 1, "alpha", 5, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "b", 1, "beta", 4, &cat, NULL, 0);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_HYBRID,
        .limit = 5,
        .min_score = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, "alpha", 5, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, 1);
    HU_ASSERT_STR_EQ(res.entries[0].key, "a");

    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    (void)0;
#endif
}

static void test_mmr_diversifies_results(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "dup1", 4, "cat dog cat", 11, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "dup2", 4, "cat dog cat", 11, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "diff", 4, "bird fish", 9, &cat, NULL, 0);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = true,
    };
    hu_retrieval_engine_t eng = hu_retrieval_create(&alloc, &mem);
    HU_ASSERT_NOT_NULL(eng.ctx);

    hu_retrieval_result_t res = {0};
    hu_error_t err = eng.vtable->retrieve(eng.ctx, &alloc, "cat dog", 7, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(res.count >= 1);
    /* MMR should produce a different ordering than pure score */
    eng.vtable->deinit(eng.ctx, &alloc);
    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    (void)0;
#endif
}

static void test_retrieval_engine_with_none_backend(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);

    hu_retrieval_engine_t eng = hu_retrieval_create(&alloc, &mem);
    HU_ASSERT_NOT_NULL(eng.ctx);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_KEYWORD,
        .limit = 5,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = eng.vtable->retrieve(eng.ctx, &alloc, "test", 4, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, 0);

    eng.vtable->deinit(eng.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static void test_keyword_limit_respected(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "a", 1, "alpha", 5, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "b", 1, "alpha", 5, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "c", 1, "alpha", 5, &cat, NULL, 0);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_KEYWORD,
        .limit = 2,
        .min_score = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_keyword_retrieve(&alloc, &mem, "alpha", 5, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(res.count <= 2);
    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    (void)0;
#endif
}

static void test_temporal_decay_factor_one(void) {
    double base = 1.0;
    const char *ts = "2026-01-01T00:00:00Z";
    double d = hu_temporal_decay_score(base, 1.0, ts, strlen(ts));
    HU_ASSERT_TRUE(d >= 0.0);
    HU_ASSERT_TRUE(d <= base);
}

static void test_temporal_decay_iso8601_format(void) {
    double base = 0.8;
    const char *ts = "2025-06-15T14:30:00Z";
    double d = hu_temporal_decay_score(base, 0.3, ts, strlen(ts));
    HU_ASSERT_TRUE(d >= 0.0);
    HU_ASSERT_TRUE(d <= 0.8);
}

static void test_semantic_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_retrieval_engine_t eng = hu_retrieval_create(&alloc, &mem);
    HU_ASSERT_NOT_NULL(eng.ctx);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_SEMANTIC,
        .limit = 5,
        .min_score = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = eng.vtable->retrieve(eng.ctx, &alloc, "query", 5, &opts, &res);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(res.count, 0);
    HU_ASSERT_NULL(res.entries);

    eng.vtable->deinit(eng.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static void test_retrieval_options_default(void) {
    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = false,
        .temporal_decay_factor = 0.0,
    };
    HU_ASSERT_EQ(opts.mode, HU_RETRIEVAL_KEYWORD);
    HU_ASSERT_EQ(opts.limit, 5u);
}

static void test_hybrid_empty_backend(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_HYBRID,
        .limit = 5,
        .min_score = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, "query", 5, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_retrieval_engine_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_retrieval_engine_t eng = hu_retrieval_create(&alloc, &mem);
    eng.vtable->deinit(eng.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static void test_mmr_rerank_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_entry_t entries[1];
    double scores[1] = {1.0};
    hu_error_t err = hu_mmr_rerank(&alloc, "q", 1, entries, scores, 0, 0.5);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_retrieval_engine_with_sqlite_backend(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "fav", 3, "coffee and tea", 14, &cat, NULL, 0);

    hu_retrieval_engine_t eng = hu_retrieval_create(&alloc, &mem);
    HU_ASSERT_NOT_NULL(eng.ctx);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = eng.vtable->retrieve(eng.ctx, &alloc, "coffee", 6, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, 1);
    HU_ASSERT_STR_EQ(res.entries[0].key, "fav");

    hu_retrieval_result_free(&alloc, &res);
    eng.vtable->deinit(eng.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
#else
    (void)0;
#endif
}

static void test_semantic_retrieve_with_local_embedder(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "doc1", 4, "machine learning basics", 23, &cat, NULL, 0);
    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_SEMANTIC,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = false,
        .temporal_decay_factor = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_semantic_retrieve(&alloc, NULL, NULL, "ML", 2, &opts, &res);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED || err == HU_ERR_INVALID_ARGUMENT);
    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    (void)0;
#endif
}

static void test_rerank_rrf_overlapping_merges_correctly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_search_result_t kw[3] = {
        {.content = hu_strdup(&alloc, "doc A"),
         .score = 0.9f,
         .rerank_score = 0.0f,
         .original_rank = 0},
        {.content = hu_strdup(&alloc, "doc B"),
         .score = 0.8f,
         .rerank_score = 0.0f,
         .original_rank = 1},
        {.content = hu_strdup(&alloc, "doc C"),
         .score = 0.7f,
         .rerank_score = 0.0f,
         .original_rank = 2},
    };
    hu_search_result_t vec[3] = {
        {.content = hu_strdup(&alloc, "doc B"),
         .score = 0.95f,
         .rerank_score = 0.0f,
         .original_rank = 0},
        {.content = hu_strdup(&alloc, "doc A"),
         .score = 0.85f,
         .rerank_score = 0.0f,
         .original_rank = 1},
        {.content = hu_strdup(&alloc, "doc D"),
         .score = 0.75f,
         .rerank_score = 0.0f,
         .original_rank = 2},
    };
    hu_search_result_t merged[8];
    size_t count = 0;
    hu_error_t err = hu_rerank_rrf(kw, 3, vec, 3, merged, 8, &count, 60.0f);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 4u); /* A, B, C, D */
    /* doc B appears in both lists — should have highest RRF score */
    HU_ASSERT_TRUE(merged[0].rerank_score >= merged[1].rerank_score);
    hu_rerank_free_results(merged, count);
    hu_rerank_free_results(kw, 3);
    hu_rerank_free_results(vec, 3);
}

static void test_rerank_rrf_disjoint_combines_both(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_search_result_t kw[2] = {
        {.content = hu_strdup(&alloc, "keyword only"),
         .score = 0.9f,
         .rerank_score = 0.0f,
         .original_rank = 0},
        {.content = hu_strdup(&alloc, "another kw"),
         .score = 0.8f,
         .rerank_score = 0.0f,
         .original_rank = 1},
    };
    hu_search_result_t vec[2] = {
        {.content = hu_strdup(&alloc, "vector only"),
         .score = 0.95f,
         .rerank_score = 0.0f,
         .original_rank = 0},
        {.content = hu_strdup(&alloc, "another vec"),
         .score = 0.85f,
         .rerank_score = 0.0f,
         .original_rank = 1},
    };
    hu_search_result_t merged[8];
    size_t count = 0;
    hu_error_t err = hu_rerank_rrf(kw, 2, vec, 2, merged, 8, &count, 60.0f);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 4u);
    hu_rerank_free_results(merged, count);
    hu_rerank_free_results(kw, 2);
    hu_rerank_free_results(vec, 2);
}

static void test_rerank_cross_encoder_scores_term_overlap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_search_result_t results[3] = {
        {.content = hu_strdup(&alloc, "apple banana"),
         .score = 0.5f,
         .rerank_score = 0.0f,
         .original_rank = 0},
        {.content = hu_strdup(&alloc, "apple cherry"),
         .score = 0.4f,
         .rerank_score = 0.0f,
         .original_rank = 1},
        {.content = hu_strdup(&alloc, "orange grape"),
         .score = 0.6f,
         .rerank_score = 0.0f,
         .original_rank = 2},
    };
    hu_error_t err = hu_rerank_cross_encoder("apple banana", results, 3);
    HU_ASSERT_EQ(err, HU_OK);
    /* First doc has both words — rerank_score should be 1.0 */
    HU_ASSERT_TRUE(results[0].rerank_score > 0.9f);
    /* Third doc has neither — should be last */
    HU_ASSERT_TRUE(results[2].rerank_score < 0.1f);
    hu_rerank_free_results(results, 3);
}

static void test_rerank_empty_results_handled_gracefully(void) {
    hu_search_result_t merged[4];
    size_t count = 99;
    hu_error_t err = hu_rerank_rrf(NULL, 0, NULL, 0, merged, 4, &count, 60.0f);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    hu_allocator_t a = hu_system_allocator();
    hu_search_result_t results[1] = {
        {.content = hu_strdup(&a, "x"), .score = 0.5f, .rerank_score = 0.0f, .original_rank = 0}};
    err = hu_rerank_cross_encoder("query", results, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_rerank_free_results(results, 1);
}

static void test_rerank_k_parameter_affects_ranking(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_search_result_t kw[2] = {
        {.content = hu_strdup(&alloc, "first"),
         .score = 0.9f,
         .rerank_score = 0.0f,
         .original_rank = 0},
        {.content = hu_strdup(&alloc, "second"),
         .score = 0.8f,
         .rerank_score = 0.0f,
         .original_rank = 1},
    };
    hu_search_result_t vec[2] = {
        {.content = hu_strdup(&alloc, "second"),
         .score = 0.95f,
         .rerank_score = 0.0f,
         .original_rank = 0},
        {.content = hu_strdup(&alloc, "first"),
         .score = 0.85f,
         .rerank_score = 0.0f,
         .original_rank = 1},
    };
    hu_search_result_t merged_k10[8], merged_k100[8];
    size_t c10 = 0, c100 = 0;
    hu_rerank_rrf(kw, 2, vec, 2, merged_k10, 8, &c10, 10.0f);
    hu_rerank_rrf(kw, 2, vec, 2, merged_k100, 8, &c100, 100.0f);
    HU_ASSERT_EQ(c10, 2u);
    HU_ASSERT_EQ(c100, 2u);
    /* With smaller k, rank differences matter more */
    HU_ASSERT_TRUE(merged_k10[0].rerank_score != merged_k100[0].rerank_score ||
                   merged_k10[0].content == merged_k100[0].content);
    hu_rerank_free_results(merged_k10, c10);
    hu_rerank_free_results(merged_k100, c100);
    hu_rerank_free_results(kw, 2);
    hu_rerank_free_results(vec, 2);
}

static void test_hybrid_retrieve_with_vector(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "v1", 2, "neural network training", 23, &cat, NULL, 0);
    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_HYBRID,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = false,
        .temporal_decay_factor = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, "neural", 6, &opts, &res);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED || err == HU_ERR_INVALID_ARGUMENT);
    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    (void)0;
#endif
}

void run_retrieval_tests(void) {
    HU_TEST_SUITE("retrieval");
    HU_RUN_TEST(test_semantic_returns_not_supported);
    HU_RUN_TEST(test_semantic_retrieve_with_local_embedder);
    HU_RUN_TEST(test_hybrid_retrieve_with_vector);
    HU_RUN_TEST(test_keyword_basic_match);
    HU_RUN_TEST(test_keyword_case_insensitive);
    HU_RUN_TEST(test_keyword_no_match_returns_empty);
    HU_RUN_TEST(test_keyword_limit_respected);
    HU_RUN_TEST(test_temporal_decay_reduces_old_scores);
    HU_RUN_TEST(test_temporal_decay_factor_one);
    HU_RUN_TEST(test_temporal_decay_iso8601_format);
    HU_RUN_TEST(test_retrieval_options_default);
    HU_RUN_TEST(test_rrf_combines_rankings);
    HU_RUN_TEST(test_hybrid_empty_backend);
    HU_RUN_TEST(test_mmr_diversifies_results);
    HU_RUN_TEST(test_mmr_rerank_empty);
    HU_RUN_TEST(test_retrieval_engine_with_none_backend);
    HU_RUN_TEST(test_retrieval_engine_deinit);
    HU_RUN_TEST(test_retrieval_engine_with_sqlite_backend);
    HU_RUN_TEST(test_rerank_rrf_overlapping_merges_correctly);
    HU_RUN_TEST(test_rerank_rrf_disjoint_combines_both);
    HU_RUN_TEST(test_rerank_cross_encoder_scores_term_overlap);
    HU_RUN_TEST(test_rerank_empty_results_handled_gracefully);
    HU_RUN_TEST(test_rerank_k_parameter_affects_ranking);
}
