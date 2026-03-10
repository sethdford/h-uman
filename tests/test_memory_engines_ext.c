/* Extended memory engine tests — postgres, redis, api, lucid, lancedb.
 * Lucid/LanceDB use in-memory mock when HU_IS_TEST. Run with engines enabled:
 * cmake -DSC_ENABLE_POSTGRES=ON -DSC_ENABLE_REDIS_ENGINE=ON -DSC_ENABLE_API_ENGINE=ON .. */

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "test_framework.h"
#include <string.h>

#if (defined(HU_IS_TEST) && HU_IS_TEST) || defined(HU_ENABLE_POSTGRES) ||  \
    defined(HU_HAS_REDIS_ENGINE) || defined(HU_HAS_API_ENGINE) ||          \
    (defined(HU_HAS_LUCID_ENGINE) && defined(HU_IS_TEST) && HU_IS_TEST) || \
    (defined(HU_HAS_LANCEDB_ENGINE) && defined(HU_IS_TEST) && HU_IS_TEST)

#if defined(HU_HAS_LUCID_ENGINE) && defined(HU_IS_TEST) && HU_IS_TEST
static void test_lucid_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_lucid_memory_create(&alloc, "/tmp/lucid.db", "/tmp/workspace");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "lk1", 3, "lucid data", 10, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t entry = {0};
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "lk1", 3, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(found);

    size_t count = 0;
    mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(count, 1u);

    HU_ASSERT(mem.vtable->health_check(mem.ctx));
    hu_memory_entry_free_fields(&alloc, &entry);
    mem.vtable->deinit(mem.ctx);
}
#endif

#if defined(HU_HAS_LANCEDB_ENGINE) && defined(HU_IS_TEST) && HU_IS_TEST
static void test_lancedb_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_lancedb_memory_create(&alloc, "/tmp/lance.db");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "vk1", 3, "vector data", 11, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = 0;
    mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(count, 1u);

    bool deleted = false;
    mem.vtable->forget(mem.ctx, "vk1", 3, &deleted);
    HU_ASSERT(deleted);

    mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(count, 0u);

    mem.vtable->deinit(mem.ctx);
}
#endif

#if defined(HU_IS_TEST) && HU_IS_TEST
static void test_postgres_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem =
        hu_postgres_memory_create(&alloc, "postgres://localhost/test", "public", "memories");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "key1", 4, "hello", 5, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t entry = {0};
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "key1", 4, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(found);
    HU_ASSERT_EQ(entry.key_len, 4u);
    HU_ASSERT_EQ(entry.content_len, 5u);

    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);

    hu_memory_entry_t *recall_out = NULL;
    size_t recall_count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "hel", 3, 10, NULL, 0, &recall_out, &recall_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(recall_count >= 1u);
    if (recall_out) {
        for (size_t i = 0; i < recall_count; i++)
            hu_memory_entry_free_fields(&alloc, &recall_out[i]);
        alloc.free(alloc.ctx, recall_out, recall_count * sizeof(hu_memory_entry_t));
    }

    hu_memory_entry_t *list_out = NULL;
    size_t list_count = 0;
    err = mem.vtable->list(mem.ctx, &alloc, &cat, NULL, 0, &list_out, &list_count);
    HU_ASSERT_EQ(err, HU_OK);
    if (list_out) {
        for (size_t i = 0; i < list_count; i++)
            hu_memory_entry_free_fields(&alloc, &list_out[i]);
        alloc.free(alloc.ctx, list_out, list_count * sizeof(hu_memory_entry_t));
    }

    hu_memory_entry_free_fields(&alloc, &entry);

    bool deleted = false;
    err = mem.vtable->forget(mem.ctx, "key1", 4, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(deleted);

    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    HU_ASSERT(mem.vtable->health_check(mem.ctx));
    mem.vtable->deinit(mem.ctx);
}
#endif

#if defined(HU_IS_TEST) && HU_IS_TEST
static void test_redis_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_redis_memory_create(&alloc, "localhost", 6379, "mem");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "key1", 4, "hello", 5, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t entry = {0};
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "key1", 4, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(found);
    HU_ASSERT_EQ(entry.key_len, 4u);
    HU_ASSERT_EQ(entry.content_len, 5u);

    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);

    hu_memory_entry_t *recall_out = NULL;
    size_t recall_count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "hel", 3, 10, NULL, 0, &recall_out, &recall_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(recall_count >= 1u);
    if (recall_out) {
        for (size_t i = 0; i < recall_count; i++)
            hu_memory_entry_free_fields(&alloc, &recall_out[i]);
        alloc.free(alloc.ctx, recall_out, recall_count * sizeof(hu_memory_entry_t));
    }

    hu_memory_entry_free_fields(&alloc, &entry);

    bool deleted = false;
    err = mem.vtable->forget(mem.ctx, "key1", 4, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(deleted);

    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    HU_ASSERT(mem.vtable->health_check(mem.ctx));
    mem.vtable->deinit(mem.ctx);
}
#endif

#if defined(HU_IS_TEST) && HU_IS_TEST
static void test_api_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_api_memory_create(&alloc, "https://api.example.com", "test-key", 5000);
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "key1", 4, "hello", 5, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t entry = {0};
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "key1", 4, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(found);
    HU_ASSERT_EQ(entry.key_len, 4u);
    HU_ASSERT_EQ(entry.content_len, 5u);

    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);

    hu_memory_entry_t *recall_out = NULL;
    size_t recall_count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "hel", 3, 10, NULL, 0, &recall_out, &recall_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(recall_count >= 1u);
    if (recall_out) {
        for (size_t i = 0; i < recall_count; i++)
            hu_memory_entry_free_fields(&alloc, &recall_out[i]);
        alloc.free(alloc.ctx, recall_out, recall_count * sizeof(hu_memory_entry_t));
    }

    hu_memory_entry_free_fields(&alloc, &entry);

    bool deleted = false;
    err = mem.vtable->forget(mem.ctx, "key1", 4, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(deleted);

    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    HU_ASSERT(mem.vtable->health_check(mem.ctx));
    mem.vtable->deinit(mem.ctx);
}
#endif

static void test_postgres_forget_nonexistent(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem =
        hu_postgres_memory_create(&alloc, "postgres://localhost/test", "public", "memories");
    HU_ASSERT_NOT_NULL(mem.vtable);

    bool deleted = false;
    hu_error_t err = mem.vtable->forget(mem.ctx, "nonexistent", 11, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(deleted);

    mem.vtable->deinit(mem.ctx);
#endif
}

#if defined(HU_IS_TEST) && HU_IS_TEST
static void test_postgres_get_nonexistent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem =
        hu_postgres_memory_create(&alloc, "postgres://localhost/test", "public", "memories");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_entry_t entry = {0};
    bool found = false;
    hu_error_t err = mem.vtable->get(mem.ctx, &alloc, "nonexistent", 11, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(found);

    mem.vtable->deinit(mem.ctx);
}

static void test_postgres_recall_empty_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem =
        hu_postgres_memory_create(&alloc, "postgres://localhost/test", "public", "memories");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "r1", 2, "content", 7, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "", 0, 10, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    (void)count; /* empty query may return 0 or all; implementation-defined */
    if (out) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(&alloc, &out[i]);
        alloc.free(alloc.ctx, out, count * sizeof(hu_memory_entry_t));
    }

    {
        bool d;
        mem.vtable->forget(mem.ctx, "r1", 2, &d);
    }
    mem.vtable->deinit(mem.ctx);
}

static void test_redis_get_nonexistent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_redis_memory_create(&alloc, "localhost", 6379, "mem");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_entry_t entry = {0};
    bool found = false;
    hu_error_t err = mem.vtable->get(mem.ctx, &alloc, "nonexistent", 11, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(found);

    mem.vtable->deinit(mem.ctx);
}

static void test_redis_count_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_redis_memory_create(&alloc, "localhost", 6379, "mem");
    HU_ASSERT_NOT_NULL(mem.vtable);

    size_t count = 0;
    hu_error_t err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    mem.vtable->deinit(mem.ctx);
}
#endif

static void test_redis_forget_nonexistent(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_redis_memory_create(&alloc, "localhost", 6379, "mem");
    HU_ASSERT_NOT_NULL(mem.vtable);

    bool deleted = false;
    hu_error_t err = mem.vtable->forget(mem.ctx, "nonexistent", 11, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(deleted);

    mem.vtable->deinit(mem.ctx);
#endif
}

static void test_api_forget_nonexistent(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_api_memory_create(&alloc, "https://api.example.com", "key", 5000);
    HU_ASSERT_NOT_NULL(mem.vtable);

    bool deleted = false;
    hu_error_t err = mem.vtable->forget(mem.ctx, "nonexistent", 11, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(deleted);

    mem.vtable->deinit(mem.ctx);
#endif
}

#endif /* engine defined */

void run_memory_engines_ext_tests(void) {
#if (defined(HU_IS_TEST) && HU_IS_TEST) || defined(HU_ENABLE_POSTGRES) ||  \
    defined(HU_HAS_REDIS_ENGINE) || defined(HU_HAS_API_ENGINE) ||          \
    (defined(HU_HAS_LUCID_ENGINE) && defined(HU_IS_TEST) && HU_IS_TEST) || \
    (defined(HU_HAS_LANCEDB_ENGINE) && defined(HU_IS_TEST) && HU_IS_TEST)
    HU_TEST_SUITE("Memory engines ext");

#if defined(HU_HAS_LUCID_ENGINE) && defined(HU_IS_TEST) && HU_IS_TEST
    HU_RUN_TEST(test_lucid_lifecycle);
#endif
#if defined(HU_HAS_LANCEDB_ENGINE) && defined(HU_IS_TEST) && HU_IS_TEST
    HU_RUN_TEST(test_lancedb_lifecycle);
#endif
#if defined(HU_IS_TEST) && HU_IS_TEST
    HU_RUN_TEST(test_postgres_lifecycle);
    HU_RUN_TEST(test_postgres_forget_nonexistent);
    HU_RUN_TEST(test_postgres_get_nonexistent);
    HU_RUN_TEST(test_postgres_recall_empty_query);
    HU_RUN_TEST(test_redis_lifecycle);
    HU_RUN_TEST(test_redis_forget_nonexistent);
    HU_RUN_TEST(test_redis_get_nonexistent);
    HU_RUN_TEST(test_redis_count_empty);
    HU_RUN_TEST(test_api_lifecycle);
    HU_RUN_TEST(test_api_forget_nonexistent);
#endif

#endif /* engine defined */
}
