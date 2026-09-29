#include "human/core/allocator.h"
#include "human/memory/lifecycle/semantic_cache.h"
#include "human/memory/vector/embeddings.h"
#include "human/memory/vector/embeddings_gemini.h"
#include "human/memory/vector/embeddings_ollama.h"
#include "human/memory/vector/embeddings_voyage.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void test_embedding_mock_and_free(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedding_provider_t p = hu_embedding_provider_noop_create(&alloc);
    HU_ASSERT_NOT_NULL(p.vtable);
    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "none");

    hu_embedding_provider_result_t res = {0};
    hu_error_t err = p.vtable->embed(p.ctx, &alloc, "hello", 5, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.dimensions, 0);
    HU_ASSERT_TRUE(res.values == NULL || res.dimensions == 0);

    hu_embedding_provider_free(&alloc, &res);
    p.vtable->deinit(p.ctx, &alloc);
}

static void test_embedding_free_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedding_provider_result_t res = {0};
    hu_embedding_provider_free(&alloc, &res);
    /* Should not crash */
}

static void test_gemini_create_and_embed_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedding_provider_t p = hu_embedding_gemini_create(&alloc, "test-key", "", 0);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "gemini");

#if HU_IS_TEST
    hu_embedding_provider_result_t res = {0};
    hu_error_t err = p.vtable->embed(p.ctx, &alloc, "test", 4, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.dimensions, 3);
    HU_ASSERT_FLOAT_EQ(res.values[0], 0.1f, 0.01f);
    HU_ASSERT_FLOAT_EQ(res.values[1], 0.2f, 0.01f);
    HU_ASSERT_FLOAT_EQ(res.values[2], 0.3f, 0.01f);
    hu_embedding_provider_free(&alloc, &res);
#endif

    p.vtable->deinit(p.ctx, &alloc);
}

static void test_ollama_create_and_embed_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedding_provider_t p = hu_embedding_ollama_create(&alloc, "", 0);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "ollama");

#if HU_IS_TEST
    hu_embedding_provider_result_t res = {0};
    hu_error_t err = p.vtable->embed(p.ctx, &alloc, "hi", 2, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.dimensions, 3);
    hu_embedding_provider_free(&alloc, &res);
#endif

    p.vtable->deinit(p.ctx, &alloc);
}

static void test_voyage_create_and_embed_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedding_provider_t p = hu_embedding_voyage_create(&alloc, "key", "", 0);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "voyage");

#if HU_IS_TEST
    hu_embedding_provider_result_t res = {0};
    hu_error_t err = p.vtable->embed(p.ctx, &alloc, "x", 1, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.dimensions, 3);
    hu_embedding_provider_free(&alloc, &res);
#endif

    p.vtable->deinit(p.ctx, &alloc);
}

static void test_semantic_cache_put_then_get_exact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedding_provider_t prov = hu_embedding_provider_noop_create(&alloc);
    hu_semantic_cache_t *cache = hu_semantic_cache_create(&alloc, 60, 100, 0.99f, &prov);

    hu_semantic_cache_put(cache, &alloc, "q1", 2, "model", 5, "response", 8, 10, "query", 5);
    hu_semantic_cache_hit_t hit = {0};
    hu_error_t err = hu_semantic_cache_get(cache, &alloc, "q1", 2, NULL, 0, &hit);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(hit.response, "response");
    hu_semantic_cache_hit_free(&alloc, &hit);

    prov.vtable->deinit(prov.ctx, &alloc);
    hu_semantic_cache_destroy(&alloc, cache);
}

static void test_embedder_noop_dimensions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedding_provider_t p = hu_embedding_provider_noop_create(&alloc);
    hu_embedding_provider_result_t res = {0};
    p.vtable->embed(p.ctx, &alloc, "x", 1, &res);
    HU_ASSERT(res.dimensions == 0 || res.values == NULL);
    hu_embedding_provider_free(&alloc, &res);
    p.vtable->deinit(p.ctx, &alloc);
}

static void test_semantic_cache_hit_miss(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedding_provider_t prov = hu_embedding_provider_noop_create(&alloc);
    hu_semantic_cache_t *cache = hu_semantic_cache_create(&alloc, 60, 1000, 0.95f, &prov);
    HU_ASSERT_NOT_NULL(cache);

    hu_error_t err = hu_semantic_cache_put(cache, &alloc, "abc123", 6, "gpt-4", 5, "Hello response",
                                           14, 10, "hello query", 11);
    HU_ASSERT_EQ(err, HU_OK);

    hu_semantic_cache_hit_t hit = {0};
    err = hu_semantic_cache_get(cache, &alloc, "nonexistent", 10, NULL, 0, &hit);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    err = hu_semantic_cache_get(cache, &alloc, "abc123", 6, NULL, 0, &hit);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(hit.response);
    HU_ASSERT_STR_EQ(hit.response, "Hello response");
    HU_ASSERT_FLOAT_EQ(hit.similarity, 1.0f, 0.01f);
    hu_semantic_cache_hit_free(&alloc, &hit);

    prov.vtable->deinit(prov.ctx, &alloc);
    hu_semantic_cache_destroy(&alloc, cache);
}

void run_vector_full_tests(void) {
    HU_TEST_SUITE("Vector Full (embeddings, cache)");
    HU_RUN_TEST(test_embedding_mock_and_free);
    HU_RUN_TEST(test_embedding_free_null_safe);
    HU_RUN_TEST(test_embedder_noop_dimensions);
    HU_RUN_TEST(test_gemini_create_and_embed_mock);
    HU_RUN_TEST(test_ollama_create_and_embed_mock);
    HU_RUN_TEST(test_voyage_create_and_embed_mock);
    HU_RUN_TEST(test_semantic_cache_hit_miss);
    HU_RUN_TEST(test_semantic_cache_put_then_get_exact);
}
