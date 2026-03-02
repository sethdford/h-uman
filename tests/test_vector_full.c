#include "test_framework.h"
#include "seaclaw/memory/vector/embeddings.h"
#include "seaclaw/memory/vector/embeddings_gemini.h"
#include "seaclaw/memory/vector/embeddings_ollama.h"
#include "seaclaw/memory/vector/embeddings_voyage.h"
#include "seaclaw/memory/vector/provider_router.h"
#include "seaclaw/memory/vector/outbox.h"
#include "seaclaw/memory/vector/store.h"
#include "seaclaw/memory/vector/store_pgvector.h"
#include "seaclaw/memory/vector/store_qdrant.h"
#include "seaclaw/memory/lifecycle/semantic_cache.h"
#include "seaclaw/core/allocator.h"
#include <string.h>
#include <stdio.h>

static void test_embedding_mock_and_free(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_provider_t p = sc_embedding_provider_noop_create(&alloc);
    SC_ASSERT_NOT_NULL(p.vtable);
    SC_ASSERT_STR_EQ(p.vtable->name(p.ctx), "none");

    sc_embedding_provider_result_t res = {0};
    sc_error_t err = p.vtable->embed(p.ctx, &alloc, "hello", 5, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.dimensions, 0);
    SC_ASSERT_TRUE(res.values == NULL || res.dimensions == 0);

    sc_embedding_provider_free(&alloc, &res);
    p.vtable->deinit(p.ctx, &alloc);
}

static void test_embedding_free_null_safe(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_provider_result_t res = {0};
    sc_embedding_provider_free(&alloc, &res);
    /* Should not crash */
}

static void test_gemini_create_and_embed_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_provider_t p = sc_embedding_gemini_create(&alloc, "test-key", "", 0);
    SC_ASSERT_NOT_NULL(p.ctx);
    SC_ASSERT_STR_EQ(p.vtable->name(p.ctx), "gemini");

#if SC_IS_TEST
    sc_embedding_provider_result_t res = {0};
    sc_error_t err = p.vtable->embed(p.ctx, &alloc, "test", 4, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.dimensions, 3);
    SC_ASSERT_FLOAT_EQ(res.values[0], 0.1f, 0.01f);
    SC_ASSERT_FLOAT_EQ(res.values[1], 0.2f, 0.01f);
    SC_ASSERT_FLOAT_EQ(res.values[2], 0.3f, 0.01f);
    sc_embedding_provider_free(&alloc, &res);
#endif

    p.vtable->deinit(p.ctx, &alloc);
}

static void test_ollama_create_and_embed_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_provider_t p = sc_embedding_ollama_create(&alloc, "", 0);
    SC_ASSERT_NOT_NULL(p.ctx);
    SC_ASSERT_STR_EQ(p.vtable->name(p.ctx), "ollama");

#if SC_IS_TEST
    sc_embedding_provider_result_t res = {0};
    sc_error_t err = p.vtable->embed(p.ctx, &alloc, "hi", 2, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.dimensions, 3);
    sc_embedding_provider_free(&alloc, &res);
#endif

    p.vtable->deinit(p.ctx, &alloc);
}

static void test_voyage_create_and_embed_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_provider_t p = sc_embedding_voyage_create(&alloc, "key", "", 0);
    SC_ASSERT_NOT_NULL(p.ctx);
    SC_ASSERT_STR_EQ(p.vtable->name(p.ctx), "voyage");

#if SC_IS_TEST
    sc_embedding_provider_result_t res = {0};
    sc_error_t err = p.vtable->embed(p.ctx, &alloc, "x", 1, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.dimensions, 3);
    sc_embedding_provider_free(&alloc, &res);
#endif

    p.vtable->deinit(p.ctx, &alloc);
}

static void test_provider_router_single(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_provider_t noop = sc_embedding_provider_noop_create(&alloc);
    sc_embedding_provider_t p = sc_embedding_provider_router_create(&alloc,
        noop, NULL, 0, NULL, 0);
    SC_ASSERT_NOT_NULL(p.ctx);
    SC_ASSERT_STR_EQ(p.vtable->name(p.ctx), "auto");

    sc_embedding_provider_result_t res = {0};
    sc_error_t err = p.vtable->embed(p.ctx, &alloc, "route", 5, &res);
    SC_ASSERT_EQ(err, SC_OK);
    sc_embedding_provider_free(&alloc, &res);

    p.vtable->deinit(p.ctx, &alloc);
}

static void test_outbox_enqueue_flush(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_outbox_t *ob = sc_embedding_outbox_create(&alloc);
    SC_ASSERT_NOT_NULL(ob);

    sc_error_t err = sc_embedding_outbox_enqueue(ob, "id1", 3, "text one", 8);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(sc_embedding_outbox_pending_count(ob), 1);

    sc_embedding_outbox_enqueue(ob, "id2", 3, "text two", 8);
    SC_ASSERT_EQ(sc_embedding_outbox_pending_count(ob), 2);

    sc_embedding_provider_t prov = sc_embedding_provider_noop_create(&alloc);
    err = sc_embedding_outbox_flush(ob, &alloc, &prov, NULL, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(sc_embedding_outbox_pending_count(ob), 0);

    prov.vtable->deinit(prov.ctx, &alloc);
    sc_embedding_outbox_destroy(&alloc, ob);
}

static void test_vector_store_upsert_search_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_vector_store_t store = sc_vector_store_mem_vec_create(&alloc);
    SC_ASSERT_NOT_NULL(store.ctx);

    float emb[] = {1.0f, 0.0f, 0.0f};
    sc_error_t err = store.vtable->upsert(store.ctx, &alloc, "k1", 2,
        emb, 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(store.vtable->count(store.ctx), 1);

    sc_vector_search_result_t *results = NULL;
    size_t n = 0;
    err = store.vtable->search(store.ctx, &alloc, emb, 3, 5, &results, &n);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(n >= 1);
    sc_vector_search_results_free(&alloc, results, n);

    store.vtable->deinit(store.ctx, &alloc);
}

static void test_pgvector_not_supported(void) {
#if !defined(SC_ENABLE_POSTGRES)
    sc_allocator_t alloc = sc_system_allocator();
    sc_pgvector_config_t cfg = {
        .connection_url = "postgresql://localhost/test",
        .table_name = "memory_vectors",
        .dimensions = 768
    };
    sc_vector_store_t store = sc_vector_store_pgvector_create(&alloc, &cfg);
    SC_ASSERT_NOT_NULL(store.ctx);

    float emb[] = {0.1f, 0.2f, 0.3f};
    sc_error_t err = store.vtable->upsert(store.ctx, &alloc, "x", 1, emb, 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);

    store.vtable->deinit(store.ctx, &alloc);
#endif
}

static void test_qdrant_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_qdrant_config_t cfg = {
        .url = "http://localhost:6333",
        .api_key = NULL,
        .collection_name = "test",
        .dimensions = 3
    };
    sc_vector_store_t store = sc_vector_store_qdrant_create(&alloc, &cfg);
    SC_ASSERT_NOT_NULL(store.ctx);

#if SC_IS_TEST
    float q[] = {0.1f, 0.2f, 0.3f};
    sc_vector_search_result_t *results = NULL;
    size_t n = 0;
    sc_error_t err = store.vtable->search(store.ctx, &alloc, q, 3, 5, &results, &n);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(n, 0u);  /* mock returns empty */
    SC_ASSERT_NULL(results);
#endif

    store.vtable->deinit(store.ctx, &alloc);
}

static void test_outbox_multiple_enqueue(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_outbox_t *ob = sc_embedding_outbox_create(&alloc);
    SC_ASSERT_NOT_NULL(ob);

    for (int i = 0; i < 5; i++) {
        char id[16];
        snprintf(id, sizeof(id), "id%d", i);
        sc_embedding_outbox_enqueue(ob, id, strlen(id), "text", 4);
    }
    SC_ASSERT_EQ(sc_embedding_outbox_pending_count(ob), 5);

    sc_embedding_provider_t prov = sc_embedding_provider_noop_create(&alloc);
    sc_embedding_outbox_flush(ob, &alloc, &prov, NULL, NULL);
    SC_ASSERT_EQ(sc_embedding_outbox_pending_count(ob), 0);

    prov.vtable->deinit(prov.ctx, &alloc);
    sc_embedding_outbox_destroy(&alloc, ob);
}

static void test_outbox_empty_flush(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_outbox_t *ob = sc_embedding_outbox_create(&alloc);
    sc_embedding_provider_t prov = sc_embedding_provider_noop_create(&alloc);
    sc_error_t err = sc_embedding_outbox_flush(ob, &alloc, &prov, NULL, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    prov.vtable->deinit(prov.ctx, &alloc);
    sc_embedding_outbox_destroy(&alloc, ob);
}

static void test_semantic_cache_put_then_get_exact(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_provider_t prov = sc_embedding_provider_noop_create(&alloc);
    sc_semantic_cache_t *cache = sc_semantic_cache_create(&alloc, 60, 100, 0.99f, &prov);

    sc_semantic_cache_put(cache, &alloc, "q1", 2, "model", 5, "response", 8, 10, "query", 5);
    sc_semantic_cache_hit_t hit = {0};
    sc_error_t err = sc_semantic_cache_get(cache, &alloc, "q1", 2, NULL, 0, &hit);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(hit.response, "response");
    sc_semantic_cache_hit_free(&alloc, &hit);

    prov.vtable->deinit(prov.ctx, &alloc);
    sc_semantic_cache_destroy(&alloc, cache);
}

static void test_embedder_noop_dimensions(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_provider_t p = sc_embedding_provider_noop_create(&alloc);
    sc_embedding_provider_result_t res = {0};
    p.vtable->embed(p.ctx, &alloc, "x", 1, &res);
    SC_ASSERT(res.dimensions == 0 || res.values == NULL);
    sc_embedding_provider_free(&alloc, &res);
    p.vtable->deinit(p.ctx, &alloc);
}

static void test_semantic_cache_hit_miss(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedding_provider_t prov = sc_embedding_provider_noop_create(&alloc);
    sc_semantic_cache_t *cache = sc_semantic_cache_create(&alloc,
        60, 1000, 0.95f, &prov);
    SC_ASSERT_NOT_NULL(cache);

    sc_error_t err = sc_semantic_cache_put(cache, &alloc,
        "abc123", 6, "gpt-4", 5, "Hello response", 14, 10,
        "hello query", 11);
    SC_ASSERT_EQ(err, SC_OK);

    sc_semantic_cache_hit_t hit = {0};
    err = sc_semantic_cache_get(cache, &alloc, "nonexistent", 10, NULL, 0, &hit);
    SC_ASSERT_EQ(err, SC_ERR_NOT_FOUND);

    err = sc_semantic_cache_get(cache, &alloc, "abc123", 6, NULL, 0, &hit);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(hit.response);
    SC_ASSERT_STR_EQ(hit.response, "Hello response");
    SC_ASSERT_FLOAT_EQ(hit.similarity, 1.0f, 0.01f);
    sc_semantic_cache_hit_free(&alloc, &hit);

    prov.vtable->deinit(prov.ctx, &alloc);
    sc_semantic_cache_destroy(&alloc, cache);
}

void run_vector_full_tests(void) {
    SC_TEST_SUITE("Vector Full (embeddings, store, cache)");
    SC_RUN_TEST(test_embedding_mock_and_free);
    SC_RUN_TEST(test_embedding_free_null_safe);
    SC_RUN_TEST(test_embedder_noop_dimensions);
    SC_RUN_TEST(test_gemini_create_and_embed_mock);
    SC_RUN_TEST(test_ollama_create_and_embed_mock);
    SC_RUN_TEST(test_voyage_create_and_embed_mock);
    SC_RUN_TEST(test_provider_router_single);
    SC_RUN_TEST(test_outbox_enqueue_flush);
    SC_RUN_TEST(test_outbox_multiple_enqueue);
    SC_RUN_TEST(test_outbox_empty_flush);
    SC_RUN_TEST(test_vector_store_upsert_search_mock);
    SC_RUN_TEST(test_pgvector_not_supported);
    SC_RUN_TEST(test_qdrant_mock);
    SC_RUN_TEST(test_semantic_cache_hit_miss);
    SC_RUN_TEST(test_semantic_cache_put_then_get_exact);
}
