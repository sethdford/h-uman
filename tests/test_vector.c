#include "test_framework.h"
#include "seaclaw/memory/vector.h"
#include "seaclaw/core/allocator.h"
#include <math.h>
#include <string.h>

static void test_cosine_similarity_identical(void) {
    float a[] = {1.0f, 2.0f, 3.0f};
    float sim = sc_cosine_similarity(a, a, 3);
    SC_ASSERT_FLOAT_EQ(sim, 1.0f, 0.001f);
}

static void test_cosine_similarity_orthogonal(void) {
    float a[] = {1.0f, 0.0f, 0.0f};
    float b[] = {0.0f, 1.0f, 0.0f};
    float sim = sc_cosine_similarity(a, b, 3);
    SC_ASSERT(fabsf(sim) < 0.001f);
}

static void test_cosine_similarity_opposite(void) {
    float a[] = {1.0f, 0.0f, 0.0f};
    float b[] = {-1.0f, 0.0f, 0.0f};
    float sim = sc_cosine_similarity(a, b, 3);
    SC_ASSERT_FLOAT_EQ(sim, -1.0f, 0.001f);
}

static void test_chunker_basic_split(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "First sentence. Second sentence. Third sentence.";
    size_t len = strlen(text);
    sc_chunker_options_t opts = { .max_chunk_size = 20, .overlap = 0 };

    sc_text_chunk_t *chunks = NULL;
    size_t count = 0;
    sc_error_t err = sc_chunker_split(&alloc, text, len, &opts, &chunks, &count);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(chunks);
    SC_ASSERT_TRUE(count >= 2);

    sc_chunker_free(&alloc, chunks, count);
}

static void test_chunker_overlap(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "One. Two. Three. Four. Five.";
    size_t len = strlen(text);
    sc_chunker_options_t opts = { .max_chunk_size = 15, .overlap = 5 };

    sc_text_chunk_t *chunks = NULL;
    size_t count = 0;
    sc_error_t err = sc_chunker_split(&alloc, text, len, &opts, &chunks, &count);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(chunks);
    SC_ASSERT_TRUE(count >= 1);

    sc_chunker_free(&alloc, chunks, count);
}

static void test_chunker_short_text(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "Hi";
    size_t len = 2;
    sc_chunker_options_t opts = { .max_chunk_size = 512, .overlap = 0 };

    sc_text_chunk_t *chunks = NULL;
    size_t count = 0;
    sc_error_t err = sc_chunker_split(&alloc, text, len, &opts, &chunks, &count);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(chunks);
    SC_ASSERT_EQ(count, 1);
    SC_ASSERT_EQ(chunks[0].text_len, 2);

    sc_chunker_free(&alloc, chunks, count);
}

static void test_vector_store_insert_search(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_vector_store_t store = sc_vector_store_mem_create(&alloc);
    SC_ASSERT_NOT_NULL(store.ctx);

    float emb1[] = {1.0f, 0.0f, 0.0f};
    float emb2[] = {0.9f, 0.1f, 0.0f};
    float emb3[] = {0.0f, 0.0f, 1.0f};
    sc_embedding_t e1 = { .values = emb1, .dim = 3 };
    sc_embedding_t e2 = { .values = emb2, .dim = 3 };
    sc_embedding_t e3 = { .values = emb3, .dim = 3 };

    sc_error_t err = store.vtable->insert(store.ctx, &alloc, "a", 1, &e1, "alpha", 5);
    SC_ASSERT_EQ(err, SC_OK);
    err = store.vtable->insert(store.ctx, &alloc, "b", 1, &e2, "beta", 4);
    SC_ASSERT_EQ(err, SC_OK);
    err = store.vtable->insert(store.ctx, &alloc, "c", 1, &e3, "gamma", 5);
    SC_ASSERT_EQ(err, SC_OK);

    sc_embedding_t query = { .values = emb1, .dim = 3 };
    sc_vector_entry_t *results = NULL;
    size_t n = 0;
    err = store.vtable->search(store.ctx, &alloc, &query, 5, &results, &n);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(n >= 1);
    SC_ASSERT_STR_EQ(results[0].id, "a");
    SC_ASSERT_TRUE(results[0].score > 0.99f);

    sc_vector_entries_free(&alloc, results, n);
    store.vtable->deinit(store.ctx, &alloc);
}

static void test_vector_store_remove(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_vector_store_t store = sc_vector_store_mem_create(&alloc);
    SC_ASSERT_NOT_NULL(store.ctx);

    float emb[] = {1.0f, 0.0f};
    sc_embedding_t e = { .values = emb, .dim = 2 };
    store.vtable->insert(store.ctx, &alloc, "x", 1, &e, "content", 7);
    SC_ASSERT_EQ(store.vtable->count(store.ctx), 1);

    sc_error_t err = store.vtable->remove(store.ctx, "x", 1);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(store.vtable->count(store.ctx), 0);

    err = store.vtable->remove(store.ctx, "y", 1);
    SC_ASSERT_EQ(err, SC_ERR_NOT_FOUND);

    store.vtable->deinit(store.ctx, &alloc);
}

static void test_cosine_similarity_unit_vector(void) {
    float a[] = {1.0f, 0.0f, 0.0f};
    float b[] = {1.0f, 0.0f, 0.0f};
    float sim = sc_cosine_similarity(a, b, 3);
    SC_ASSERT_FLOAT_EQ(sim, 1.0f, 0.001f);
}

static void test_cosine_similarity_scaled_identical(void) {
    float a[] = {2.0f, 4.0f, 6.0f};
    float b[] = {1.0f, 2.0f, 3.0f};
    float sim = sc_cosine_similarity(a, b, 3);
    SC_ASSERT_FLOAT_EQ(sim, 1.0f, 0.001f);
}

static void test_cosine_similarity_zero_vector(void) {
    float a[] = {0.0f, 0.0f, 0.0f};
    float b[] = {1.0f, 0.0f, 0.0f};
    float sim = sc_cosine_similarity(a, b, 3);
    SC_ASSERT_TRUE(sim >= -0.001f && sim <= 0.001f);
}

static void test_chunker_empty_text(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_chunker_options_t opts = { .max_chunk_size = 100, .overlap = 0 };
    sc_text_chunk_t *chunks = NULL;
    size_t count = 0;
    sc_error_t err = sc_chunker_split(&alloc, "", 0, &opts, &chunks, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count <= 1);
    if (chunks) sc_chunker_free(&alloc, chunks, count);
}

static void test_chunker_large_overlap(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "One. Two. Three.";
    sc_chunker_options_t opts = { .max_chunk_size = 10, .overlap = 8 };
    sc_text_chunk_t *chunks = NULL;
    size_t count = 0;
    sc_error_t err = sc_chunker_split(&alloc, text, strlen(text), &opts, &chunks, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(chunks);
    sc_chunker_free(&alloc, chunks, count);
}

static void test_vector_store_count(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_vector_store_t store = sc_vector_store_mem_create(&alloc);
    SC_ASSERT_EQ(store.vtable->count(store.ctx), 0);

    float emb[] = {1.0f, 0.0f};
    sc_embedding_t e = { .values = emb, .dim = 2 };
    store.vtable->insert(store.ctx, &alloc, "k", 1, &e, "content", 7);
    SC_ASSERT_EQ(store.vtable->count(store.ctx), 1);
    store.vtable->insert(store.ctx, &alloc, "k2", 2, &e, "content2", 8);
    SC_ASSERT_EQ(store.vtable->count(store.ctx), 2);

    store.vtable->deinit(store.ctx, &alloc);
}

static void test_embedder_local_basic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_embedder_t emb = sc_embedder_local_create(&alloc);
    SC_ASSERT_NOT_NULL(emb.ctx);

    SC_ASSERT_EQ(emb.vtable->dimensions(emb.ctx), SC_EMBEDDING_DIM);

    sc_embedding_t out = {0};
    sc_error_t err = emb.vtable->embed(emb.ctx, &alloc, "hello world", 11, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out.values);
    SC_ASSERT_EQ(out.dim, SC_EMBEDDING_DIM);

    /* Same input in SC_IS_TEST gives same output (deterministic) */
    sc_embedding_t out2 = {0};
    err = emb.vtable->embed(emb.ctx, &alloc, "hello world", 11, &out2);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(memcmp(out.values, out2.values, out.dim * sizeof(float)) == 0);

    sc_embedding_free(&alloc, &out);
    sc_embedding_free(&alloc, &out2);
    emb.vtable->deinit(emb.ctx, &alloc);
}

void run_vector_tests(void) {
    SC_TEST_SUITE("vector");
    SC_RUN_TEST(test_cosine_similarity_identical);
    SC_RUN_TEST(test_cosine_similarity_orthogonal);
    SC_RUN_TEST(test_cosine_similarity_opposite);
    SC_RUN_TEST(test_cosine_similarity_unit_vector);
    SC_RUN_TEST(test_cosine_similarity_scaled_identical);
    SC_RUN_TEST(test_cosine_similarity_zero_vector);
    SC_RUN_TEST(test_chunker_basic_split);
    SC_RUN_TEST(test_chunker_overlap);
    SC_RUN_TEST(test_chunker_short_text);
    SC_RUN_TEST(test_chunker_empty_text);
    SC_RUN_TEST(test_chunker_large_overlap);
    SC_RUN_TEST(test_vector_store_insert_search);
    SC_RUN_TEST(test_vector_store_remove);
    SC_RUN_TEST(test_vector_store_count);
    SC_RUN_TEST(test_embedder_local_basic);
}
