#include "test_framework.h"
#include "seaclaw/memory/vector/store_qdrant.h"
#include "seaclaw/memory/vector/store_pgvector.h"
#include "seaclaw/memory/vector/store.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>

static void test_qdrant_create_destroy(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_qdrant_config_t cfg = {
        .url = "http://localhost:6333",
        .api_key = NULL,
        .collection_name = "test_collection",
        .dimensions = 3,
    };
    sc_vector_store_t store = sc_vector_store_qdrant_create(&alloc, &cfg);
    SC_ASSERT_NOT_NULL(store.ctx);
    store.vtable->deinit(store.ctx, &alloc);
}

static void test_qdrant_upsert_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_qdrant_config_t cfg = {
        .url = "http://localhost:6333",
        .api_key = NULL,
        .collection_name = "test",
        .dimensions = 3,
    };
    sc_vector_store_t store = sc_vector_store_qdrant_create(&alloc, &cfg);
    float vec[] = {0.1f, 0.2f, 0.3f};
    sc_error_t err = store.vtable->upsert(store.ctx, &alloc, "id1", 3, vec, 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    store.vtable->deinit(store.ctx, &alloc);
}

static void test_qdrant_search_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_qdrant_config_t cfg = {
        .url = "http://localhost:6333",
        .api_key = NULL,
        .collection_name = "test",
        .dimensions = 3,
    };
    sc_vector_store_t store = sc_vector_store_qdrant_create(&alloc, &cfg);
    float vec[] = {0.1f, 0.2f, 0.3f};
    sc_vector_search_result_t *results = NULL;
    size_t count = 0;
    sc_error_t err = store.vtable->search(store.ctx, &alloc, vec, 3, 5, &results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);  /* mock returns 0 results */
    if (results && count > 0) {
        for (size_t i = 0; i < count; i++)
            if (results[i].id) alloc.free(alloc.ctx, (void *)results[i].id, strlen(results[i].id) + 1);
        alloc.free(alloc.ctx, results, count * sizeof(sc_vector_search_result_t));
    }
    store.vtable->deinit(store.ctx, &alloc);
}

static void test_qdrant_delete_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_qdrant_config_t cfg = {
        .url = "http://localhost:6333",
        .api_key = NULL,
        .collection_name = "test",
        .dimensions = 3,
    };
    sc_vector_store_t store = sc_vector_store_qdrant_create(&alloc, &cfg);
    sc_error_t err = store.vtable->delete(store.ctx, &alloc, "id1", 3);
    SC_ASSERT_EQ(err, SC_OK);
    store.vtable->deinit(store.ctx, &alloc);
}

static void test_qdrant_count_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_qdrant_config_t cfg = {
        .url = "http://localhost:6333",
        .api_key = NULL,
        .collection_name = "test",
        .dimensions = 3,
    };
    sc_vector_store_t store = sc_vector_store_qdrant_create(&alloc, &cfg);
    size_t count = store.vtable->count(store.ctx);
    SC_ASSERT_EQ(count, 0u);  /* mock returns 0 */
    store.vtable->deinit(store.ctx, &alloc);
}

static void test_qdrant_mock_upsert_search_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_qdrant_config_t cfg = {
        .url = "http://localhost:6333",
        .api_key = NULL,
        .collection_name = "test",
        .dimensions = 3,
    };
    sc_vector_store_t store = sc_vector_store_qdrant_create(&alloc, &cfg);
    float vec[] = {0.1f, 0.2f, 0.3f};
    sc_error_t err = store.vtable->upsert(store.ctx, &alloc, "id1", 3, vec, 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_vector_search_result_t *results = NULL;
    size_t count = 0;
    err = store.vtable->search(store.ctx, &alloc, vec, 3, 5, &results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(count <= 5);
    if (results) {
        for (size_t i = 0; i < count; i++)
            if (results[i].id) alloc.free(alloc.ctx, (void *)results[i].id, strlen(results[i].id) + 1);
        alloc.free(alloc.ctx, results, count * sizeof(sc_vector_search_result_t));
    }
    store.vtable->deinit(store.ctx, &alloc);
}

static void test_pgvector_create_destroy(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_pgvector_config_t cfg = {
        .connection_url = "postgresql://localhost/test",
        .table_name = "memory_vectors",
        .dimensions = 3,
    };
    sc_vector_store_t store = sc_vector_store_pgvector_create(&alloc, &cfg);
    SC_ASSERT_NOT_NULL(store.ctx);
    store.vtable->deinit(store.ctx, &alloc);
}

static void test_pgvector_ops_when_disabled(void) {
#if !defined(SC_ENABLE_POSTGRES)
    sc_allocator_t alloc = sc_system_allocator();
    sc_pgvector_config_t cfg = {
        .connection_url = "postgresql://localhost/test",
        .table_name = "memory_vectors",
        .dimensions = 3,
    };
    sc_vector_store_t store = sc_vector_store_pgvector_create(&alloc, &cfg);
    float vec[] = {0.1f, 0.2f, 0.3f};
    sc_error_t err = store.vtable->upsert(store.ctx, &alloc, "id1", 3, vec, 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
    sc_vector_search_result_t *results = NULL;
    size_t count = 0;
    err = store.vtable->search(store.ctx, &alloc, vec, 3, 5, &results, &count);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
    err = store.vtable->delete(store.ctx, &alloc, "id1", 3);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
    size_t n = store.vtable->count(store.ctx);
    SC_ASSERT_EQ(n, 0u);
    store.vtable->deinit(store.ctx, &alloc);
#endif
}

void run_vector_stores_tests(void) {
    SC_TEST_SUITE("Vector Stores");
    SC_RUN_TEST(test_qdrant_create_destroy);
    SC_RUN_TEST(test_qdrant_upsert_mock);
    SC_RUN_TEST(test_qdrant_search_mock);
    SC_RUN_TEST(test_qdrant_delete_mock);
    SC_RUN_TEST(test_qdrant_count_mock);
    SC_RUN_TEST(test_qdrant_mock_upsert_search_empty);
    SC_RUN_TEST(test_pgvector_create_destroy);
    SC_RUN_TEST(test_pgvector_ops_when_disabled);
}
