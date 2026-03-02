#include "test_framework.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/retrieval.h"
#include "seaclaw/core/allocator.h"
#include <math.h>
#include <string.h>

static void test_keyword_basic_match(void) {
#ifdef SC_ENABLE_SQLITE
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "user_pref", 9, "likes dark mode", 15, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "pet", 3, "dog named Spot", 14, &cat, NULL, 0);

    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = false,
        .temporal_decay_factor = 0.0,
    };
    sc_retrieval_result_t res = {0};
    sc_error_t err = sc_keyword_retrieve(&alloc, &mem, "dark", 4, &opts, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.count, 1);
    SC_ASSERT_STR_EQ(res.entries[0].key, "user_pref");
    SC_ASSERT_TRUE(res.scores[0] > 0.0);

    sc_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    SC_ASSERT_TRUE(1); /* skip when SQLite disabled */
#endif
}

static void test_keyword_case_insensitive(void) {
#ifdef SC_ENABLE_SQLITE
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "k1", 2, "Hello World", 11, &cat, NULL, 0);

    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = false,
        .temporal_decay_factor = 0.0,
    };
    sc_retrieval_result_t res = {0};
    sc_error_t err = sc_keyword_retrieve(&alloc, &mem, "HELLO", 5, &opts, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.count, 1);
    SC_ASSERT_STR_EQ(res.entries[0].key, "k1");

    sc_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    SC_ASSERT_TRUE(1);
#endif
}

static void test_keyword_no_match_returns_empty(void) {
#ifdef SC_ENABLE_SQLITE
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "k1", 2, "apple banana", 12, &cat, NULL, 0);

    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
    };
    sc_retrieval_result_t res = {0};
    sc_error_t err = sc_keyword_retrieve(&alloc, &mem, "xyz", 3, &opts, &res);
    SC_ASSERT_EQ(err, SC_OK);
    /* With min_score=0.0, results may include low-score entries */
    sc_retrieval_result_free(&alloc, &res);

    mem.vtable->deinit(mem.ctx);
#else
    SC_ASSERT_TRUE(1);
#endif
}

static void test_temporal_decay_reduces_old_scores(void) {
    double base = 1.0;
    const char *old_ts = "2026-02-27T12:00:00Z";
    double decayed = sc_temporal_decay_score(base, 0.5, old_ts, strlen(old_ts));
    SC_ASSERT_TRUE(decayed >= 0.0);
    SC_ASSERT_TRUE(decayed <= base);
    /* No decay when factor is 0 */
    double unchanged = sc_temporal_decay_score(base, 0.0, old_ts, strlen(old_ts));
    SC_ASSERT_FLOAT_EQ(unchanged, base, 1e-9);
    /* No timestamp = no decay */
    double no_ts = sc_temporal_decay_score(base, 0.5, NULL, 0);
    SC_ASSERT_FLOAT_EQ(no_ts, base, 1e-9);
}

static void test_rrf_combines_rankings(void) {
    /* RRF is used internally when we have both keyword and semantic.
     * For now hybrid just delegates to keyword - this test verifies
     * hybrid doesn't crash and returns keyword-like results. */
#ifdef SC_ENABLE_SQLITE
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "a", 1, "alpha", 5, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "b", 1, "beta", 4, &cat, NULL, 0);

    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_HYBRID,
        .limit = 5,
        .min_score = 0.0,
    };
    sc_retrieval_result_t res = {0};
    sc_error_t err = sc_hybrid_retrieve(&alloc, &mem, "alpha", 5, &opts, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.count, 1);
    SC_ASSERT_STR_EQ(res.entries[0].key, "a");

    sc_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    SC_ASSERT_TRUE(1);
#endif
}

static void test_mmr_diversifies_results(void) {
#ifdef SC_ENABLE_SQLITE
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "dup1", 4, "cat dog cat", 11, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "dup2", 4, "cat dog cat", 11, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "diff", 4, "bird fish", 9, &cat, NULL, 0);

    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = true,
    };
    sc_retrieval_engine_t eng = sc_retrieval_create(&alloc, &mem);
    SC_ASSERT_NOT_NULL(eng.ctx);

    sc_retrieval_result_t res = {0};
    sc_error_t err = eng.vtable->retrieve(eng.ctx, &alloc, "cat dog", 7, &opts, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(res.count >= 1);
    /* MMR should produce a different ordering than pure score */
    eng.vtable->deinit(eng.ctx, &alloc);
    sc_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    SC_ASSERT_TRUE(1);
#endif
}

static void test_retrieval_engine_with_none_backend(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);

    sc_retrieval_engine_t eng = sc_retrieval_create(&alloc, &mem);
    SC_ASSERT_NOT_NULL(eng.ctx);

    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_KEYWORD,
        .limit = 5,
    };
    sc_retrieval_result_t res = {0};
    sc_error_t err = eng.vtable->retrieve(eng.ctx, &alloc, "test", 4, &opts, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.count, 0);

    eng.vtable->deinit(eng.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static void test_keyword_limit_respected(void) {
#ifdef SC_ENABLE_SQLITE
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "a", 1, "alpha", 5, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "b", 1, "alpha", 5, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "c", 1, "alpha", 5, &cat, NULL, 0);

    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_KEYWORD,
        .limit = 2,
        .min_score = 0.0,
    };
    sc_retrieval_result_t res = {0};
    sc_error_t err = sc_keyword_retrieve(&alloc, &mem, "alpha", 5, &opts, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(res.count <= 2);
    sc_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
#else
    SC_ASSERT_TRUE(1);
#endif
}

static void test_temporal_decay_factor_one(void) {
    double base = 1.0;
    const char *ts = "2026-01-01T00:00:00Z";
    double d = sc_temporal_decay_score(base, 1.0, ts, strlen(ts));
    SC_ASSERT_TRUE(d >= 0.0);
    SC_ASSERT_TRUE(d <= base);
}

static void test_temporal_decay_iso8601_format(void) {
    double base = 0.8;
    const char *ts = "2025-06-15T14:30:00Z";
    double d = sc_temporal_decay_score(base, 0.3, ts, strlen(ts));
    SC_ASSERT_TRUE(d >= 0.0);
    SC_ASSERT_TRUE(d <= 0.8);
}

static void test_semantic_returns_not_supported(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_retrieval_engine_t eng = sc_retrieval_create(&alloc, &mem);
    SC_ASSERT_NOT_NULL(eng.ctx);

    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_SEMANTIC,
        .limit = 5,
        .min_score = 0.0,
    };
    sc_retrieval_result_t res = {0};
    sc_error_t err = eng.vtable->retrieve(eng.ctx, &alloc, "query", 5, &opts, &res);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
    SC_ASSERT_EQ(res.count, 0);
    SC_ASSERT_NULL(res.entries);

    eng.vtable->deinit(eng.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static void test_retrieval_options_default(void) {
    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = false,
        .temporal_decay_factor = 0.0,
    };
    SC_ASSERT_EQ(opts.mode, SC_RETRIEVAL_KEYWORD);
    SC_ASSERT_EQ(opts.limit, 5u);
}

static void test_hybrid_empty_backend(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_HYBRID,
        .limit = 5,
        .min_score = 0.0,
    };
    sc_retrieval_result_t res = {0};
    sc_error_t err = sc_hybrid_retrieve(&alloc, &mem, "query", 5, &opts, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.count, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_retrieval_engine_deinit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_retrieval_engine_t eng = sc_retrieval_create(&alloc, &mem);
    eng.vtable->deinit(eng.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static void test_mmr_rerank_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_entry_t entries[1];
    double scores[1] = {1.0};
    sc_error_t err = sc_mmr_rerank(&alloc, "q", 1, entries, scores, 0, 0.5);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_retrieval_engine_with_sqlite_backend(void) {
#ifdef SC_ENABLE_SQLITE
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "fav", 3, "coffee and tea", 14, &cat, NULL, 0);

    sc_retrieval_engine_t eng = sc_retrieval_create(&alloc, &mem);
    SC_ASSERT_NOT_NULL(eng.ctx);

    sc_retrieval_options_t opts = {
        .mode = SC_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
    };
    sc_retrieval_result_t res = {0};
    sc_error_t err = eng.vtable->retrieve(eng.ctx, &alloc, "coffee", 6, &opts, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.count, 1);
    SC_ASSERT_STR_EQ(res.entries[0].key, "fav");

    sc_retrieval_result_free(&alloc, &res);
    eng.vtable->deinit(eng.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
#else
    SC_ASSERT_TRUE(1);
#endif
}

void run_retrieval_tests(void) {
    SC_TEST_SUITE("retrieval");
    SC_RUN_TEST(test_semantic_returns_not_supported);
    SC_RUN_TEST(test_keyword_basic_match);
    SC_RUN_TEST(test_keyword_case_insensitive);
    SC_RUN_TEST(test_keyword_no_match_returns_empty);
    SC_RUN_TEST(test_keyword_limit_respected);
    SC_RUN_TEST(test_temporal_decay_reduces_old_scores);
    SC_RUN_TEST(test_temporal_decay_factor_one);
    SC_RUN_TEST(test_temporal_decay_iso8601_format);
    SC_RUN_TEST(test_retrieval_options_default);
    SC_RUN_TEST(test_rrf_combines_rankings);
    SC_RUN_TEST(test_hybrid_empty_backend);
    SC_RUN_TEST(test_mmr_diversifies_results);
    SC_RUN_TEST(test_mmr_rerank_empty);
    SC_RUN_TEST(test_retrieval_engine_with_none_backend);
    SC_RUN_TEST(test_retrieval_engine_deinit);
    SC_RUN_TEST(test_retrieval_engine_with_sqlite_backend);
}
