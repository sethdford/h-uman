#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/lifecycle.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define hu_mkdir(path) _mkdir(path)
#define close(fd)      _close(fd)
#else
#include <fcntl.h>
#include <unistd.h>
#define hu_mkdir(path) mkdir((path), 0755)
#endif

static void test_cache_put_get(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_cache_t *cache = hu_memory_cache_create(&alloc, 8);
    HU_ASSERT_NOT_NULL(cache);

    hu_memory_entry_t entry = {0};
    entry.key = "k1";
    entry.key_len = 2;
    entry.content = "content one";
    entry.content_len = 11;
    entry.category.tag = HU_MEMORY_CATEGORY_CORE;
    entry.timestamp = "2024-01-15T10:00:00Z";
    entry.timestamp_len = 20;

    hu_error_t err = hu_memory_cache_put(cache, "k1", 2, &entry);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_memory_cache_count(cache), 1);

    hu_memory_entry_t out = {0};
    bool found = false;
    err = hu_memory_cache_get(cache, "k1", 2, &out, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ(out.key_len, 2);
    HU_ASSERT_EQ(memcmp(out.key, "k1", 2), 0);
    HU_ASSERT_EQ(out.content_len, 11);
    HU_ASSERT_EQ(memcmp(out.content, "content one", 11), 0);

    if (out.key)
        alloc.free(alloc.ctx, (void *)out.key, out.key_len + 1);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);
    if (out.timestamp)
        alloc.free(alloc.ctx, (void *)out.timestamp, out.timestamp_len + 1);

    hu_memory_cache_destroy(cache);
}

static void test_cache_eviction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_cache_t *cache = hu_memory_cache_create(&alloc, 3);
    HU_ASSERT_NOT_NULL(cache);

    hu_memory_entry_t entry = {0};
    entry.category.tag = HU_MEMORY_CATEGORY_CORE;

    const char *keys[] = {"a", "b", "c", "d"};
    for (size_t i = 0; i < 4; i++) {
        entry.key = keys[i];
        entry.key_len = 1;
        entry.content = keys[i];
        entry.content_len = 1;
        hu_memory_cache_put(cache, keys[i], 1, &entry);
    }

    HU_ASSERT_EQ(hu_memory_cache_count(cache), 3);

    bool found = false;
    hu_memory_entry_t out = {0};
    hu_memory_cache_get(cache, "a", 1, &out, &found);
    HU_ASSERT_FALSE(found);

    hu_memory_cache_get(cache, "b", 1, &out, &found);
    HU_ASSERT_TRUE(found);
    if (out.key)
        alloc.free(alloc.ctx, (void *)out.key, out.key_len + 1);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);

    hu_memory_cache_get(cache, "c", 1, &out, &found);
    HU_ASSERT_TRUE(found);
    if (out.key)
        alloc.free(alloc.ctx, (void *)out.key, out.key_len + 1);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);

    hu_memory_cache_get(cache, "d", 1, &out, &found);
    HU_ASSERT_TRUE(found);
    if (out.key)
        alloc.free(alloc.ctx, (void *)out.key, out.key_len + 1);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);

    hu_memory_cache_destroy(cache);
}

static void test_cache_invalidate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_cache_t *cache = hu_memory_cache_create(&alloc, 4);
    HU_ASSERT_NOT_NULL(cache);

    hu_memory_entry_t entry = {0};
    entry.category.tag = HU_MEMORY_CATEGORY_CORE;
    entry.key = "x";
    entry.key_len = 1;
    entry.content = "val";
    entry.content_len = 3;

    hu_memory_cache_put(cache, "x", 1, &entry);
    hu_memory_cache_put(cache, "y", 1, &entry);
    HU_ASSERT_EQ(hu_memory_cache_count(cache), 2);

    hu_memory_cache_invalidate(cache, "x", 1);
    HU_ASSERT_EQ(hu_memory_cache_count(cache), 1);

    bool found = false;
    hu_memory_entry_t out = {0};
    hu_memory_cache_get(cache, "x", 1, &out, &found);
    HU_ASSERT_FALSE(found);
    hu_memory_cache_get(cache, "y", 1, &out, &found);
    HU_ASSERT_TRUE(found);
    if (out.key)
        alloc.free(alloc.ctx, (void *)out.key, out.key_len + 1);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);

    hu_memory_cache_destroy(cache);
}

static void test_cache_clear(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_cache_t *cache = hu_memory_cache_create(&alloc, 4);
    HU_ASSERT_NOT_NULL(cache);

    hu_memory_entry_t entry = {0};
    entry.category.tag = HU_MEMORY_CATEGORY_CORE;
    entry.key = "k";
    entry.key_len = 1;
    entry.content = "v";
    entry.content_len = 1;

    hu_memory_cache_put(cache, "a", 1, &entry);
    hu_memory_cache_put(cache, "b", 1, &entry);
    HU_ASSERT_EQ(hu_memory_cache_count(cache), 2);

    hu_memory_cache_clear(cache);
    HU_ASSERT_EQ(hu_memory_cache_count(cache), 0);

    bool found = false;
    hu_memory_entry_t out = {0};
    hu_memory_cache_get(cache, "a", 1, &out, &found);
    HU_ASSERT_FALSE(found);

    hu_memory_cache_destroy(cache);
}

void run_lifecycle_tests(void) {
    HU_TEST_SUITE("Lifecycle");
    HU_RUN_TEST(test_cache_put_get);
    HU_RUN_TEST(test_cache_eviction);
    HU_RUN_TEST(test_cache_invalidate);
    HU_RUN_TEST(test_cache_clear);
}
