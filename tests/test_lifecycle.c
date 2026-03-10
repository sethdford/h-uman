#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/lifecycle.h"
#include "human/memory/lifecycle/migrate.h"
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

static void test_hygiene_removes_oversized(void) {
    hu_allocator_t alloc = hu_system_allocator();
#ifdef HU_ENABLE_SQLITE
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
#else
    hu_memory_t mem = hu_none_memory_create(&alloc);
#endif
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
#ifdef HU_ENABLE_SQLITE
    char big[1025];
    memset(big, 'x', 1024);
    big[1024] = '\0';
    mem.vtable->store(mem.ctx, "big_key", 7, big, 1024, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "small", 5, "ok", 2, &cat, NULL, 0);
#endif

    hu_hygiene_config_t config = {
        .max_entries = 0,
        .max_entry_size = 512,
        .max_age_seconds = 0,
        .deduplicate = false,
    };
    hu_hygiene_stats_t stats = {0};
    hu_error_t err = hu_memory_hygiene_run(&alloc, &mem, &config, &stats);
    HU_ASSERT_EQ(err, HU_OK);

#ifdef HU_ENABLE_SQLITE
    HU_ASSERT_TRUE(stats.entries_scanned >= 1);
    HU_ASSERT_TRUE(stats.oversized_removed >= 1 || stats.entries_removed >= 1);
#endif

    mem.vtable->deinit(mem.ctx);
}

static void test_hygiene_removes_expired(void) {
    hu_allocator_t alloc = hu_system_allocator();
#ifdef HU_ENABLE_SQLITE
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
#else
    hu_memory_t mem = hu_none_memory_create(&alloc);
#endif
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
#ifdef HU_ENABLE_SQLITE
    mem.vtable->store(mem.ctx, "old", 3, "old content", 11, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "new", 3, "new content", 11, &cat, NULL, 0);

    hu_hygiene_config_t config = {
        .max_entries = 0,
        .max_entry_size = 0,
        .max_age_seconds = 1,
        .deduplicate = false,
    };
    hu_hygiene_stats_t stats = {0};
    hu_error_t err = hu_memory_hygiene_run(&alloc, &mem, &config, &stats);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(stats.entries_scanned >= 1);

    sleep(2);
    hu_hygiene_stats_t stats2 = {0};
    err = hu_memory_hygiene_run(&alloc, &mem, &config, &stats2);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(stats2.expired_removed >= 1 || stats2.entries_removed >= 1);
#endif

    mem.vtable->deinit(mem.ctx);
}

static void test_hygiene_deduplicates(void) {
    hu_allocator_t alloc = hu_system_allocator();
#ifdef HU_ENABLE_SQLITE
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
#else
    hu_memory_t mem = hu_none_memory_create(&alloc);
#endif
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
#ifdef HU_ENABLE_SQLITE
    mem.vtable->store(mem.ctx, "dup_a", 5, "same content", 12, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "dup_b", 5, "same content", 12, &cat, NULL, 0);
#endif

    hu_hygiene_config_t config = {
        .max_entries = 0,
        .max_entry_size = 0,
        .max_age_seconds = 0,
        .deduplicate = true,
    };
    hu_hygiene_stats_t stats = {0};
    hu_error_t err = hu_memory_hygiene_run(&alloc, &mem, &config, &stats);
    HU_ASSERT_EQ(err, HU_OK);

#ifdef HU_ENABLE_SQLITE
    HU_ASSERT_TRUE(stats.entries_scanned >= 1);
    if (stats.entries_scanned >= 2)
        HU_ASSERT_TRUE(stats.duplicates_removed >= 1 || stats.entries_removed >= 1);
#endif

    mem.vtable->deinit(mem.ctx);
}

static void test_snapshot_export_import(void) {
    hu_allocator_t alloc = hu_system_allocator();
#ifdef HU_ENABLE_SQLITE
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
#else
    hu_memory_t mem = hu_none_memory_create(&alloc);
#endif
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
#ifdef HU_ENABLE_SQLITE
    mem.vtable->store(mem.ctx, "snap_key1", 9, "snap_val1", 9, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "snap_key2", 9, "snap_val2", 9, &cat, NULL, 0);
#endif

    char path_buf[] = "/tmp/human_snap_XXXXXX";
#ifdef _WIN32
    (void)path_buf;
    const char *path = "human_snap_test.json";
    size_t path_len = strlen(path);
#else
    int fd = mkstemp(path_buf);
    HU_ASSERT_TRUE(fd >= 0);
    close(fd);
    const char *path = path_buf;
    size_t path_len = strlen(path);
#endif

    hu_error_t err = hu_memory_snapshot_export(&alloc, &mem, path, path_len);
    HU_ASSERT_EQ(err, HU_OK);

#ifdef HU_ENABLE_SQLITE
    hu_memory_t mem2 = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem2.ctx);

    err = hu_memory_snapshot_import(&alloc, &mem2, path, path_len);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    err = mem2.vtable->list(mem2.ctx, &alloc, NULL, NULL, 0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);

    bool found_any = false;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].key_len == 9) {
            if (memcmp(entries[i].key, "snap_key1", 9) == 0 ||
                memcmp(entries[i].key, "snap_key2", 9) == 0)
                found_any = true;
        }
        hu_memory_entry_free_fields(&alloc, &entries[i]);
    }
    if (entries)
        alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));
    HU_ASSERT_TRUE(found_any);

    mem2.vtable->deinit(mem2.ctx);
#endif

#ifndef _WIN32
    unlink(path);
#endif
    mem.vtable->deinit(mem.ctx);
}

static void test_summarizer_truncation(void) {
    hu_allocator_t alloc = hu_system_allocator();
#ifdef HU_ENABLE_SQLITE
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
#else
    hu_memory_t mem = hu_none_memory_create(&alloc);
#endif
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
#ifdef HU_ENABLE_SQLITE
    char long_content[256];
    memset(long_content, 'a', 255);
    long_content[255] = '\0';
    mem.vtable->store(mem.ctx, "long_entry", 10, long_content, 255, &cat, NULL, 0);
#endif

    hu_summarizer_config_t config = {
        .batch_size = 5,
        .max_summary_len = 50,
        .provider = NULL,
    };
    hu_summarizer_stats_t stats = {0};
    hu_error_t err = hu_memory_summarize(&alloc, &mem, &config, &stats);
    HU_ASSERT_EQ(err, HU_OK);

#ifdef HU_ENABLE_SQLITE
    HU_ASSERT_EQ(stats.entries_summarized, 1);
    HU_ASSERT_TRUE(stats.tokens_saved > 0);

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    err = mem.vtable->list(mem.ctx, &alloc, NULL, NULL, 0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_EQ(entries[0].content_len, 50);
    hu_memory_entry_free_fields(&alloc, &entries[0]);
    alloc.free(alloc.ctx, entries, sizeof(hu_memory_entry_t));
#endif

    mem.vtable->deinit(mem.ctx);
}

/* ─── Migrate: brain.db read and free ───────────────────────────────────────── */
static void test_migrate_invalid_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sqlite_source_entry_t *out = NULL;
    size_t count = 0;
    hu_error_t err = hu_migrate_read_brain_db(NULL, "/tmp/x.db", &out, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    err = hu_migrate_read_brain_db(&alloc, NULL, &out, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_migrate_free_entries_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migrate_free_entries(&alloc, NULL, 0);
    hu_migrate_free_entries(NULL, NULL, 0);
}

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

static void test_migrate_read_nonexistent_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sqlite_source_entry_t *out = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_migrate_read_brain_db(&alloc, "/tmp/human_nonexistent_migrate_12345.db", &out, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_migrate_read_brain_db_with_temp(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path_buf[] = "/tmp/human_migrate_XXXXXX";
#if defined(_WIN32) || defined(_WIN64)
    (void)path_buf;
    (void)alloc;
    return;
#else
    int fd = mkstemp(path_buf);
    HU_ASSERT_TRUE(fd >= 0);
    close(fd);

    sqlite3 *db = NULL;
    if (sqlite3_open(path_buf, &db) != SQLITE_OK) {
        unlink(path_buf);
        return;
    }
    const char *sql = "CREATE TABLE memories (key TEXT, content TEXT, category TEXT);"
                      "INSERT INTO memories VALUES ('k1','content one','core');";
    char *errmsg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
        sqlite3_close(db);
        unlink(path_buf);
        return;
    }
    sqlite3_close(db);

    hu_sqlite_source_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_migrate_read_brain_db(&alloc, path_buf, &entries, &count);
    unlink(path_buf);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(entries[0].key);
    HU_ASSERT_NOT_NULL(entries[0].content);
    HU_ASSERT_STR_EQ(entries[0].key, "k1");
    HU_ASSERT_STR_EQ(entries[0].content, "content one");
    HU_ASSERT_EQ(entries[0].category ? strcmp(entries[0].category, "core") : 0, 0);

    hu_migrate_free_entries(&alloc, entries, count);
#endif
}
#else
static void test_migrate_not_supported_without_sqlite(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sqlite_source_entry_t *out = NULL;
    size_t count = 0;
    hu_error_t err = hu_migrate_read_brain_db(&alloc, "/tmp/any.db", &out, &count);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}
#endif

void run_lifecycle_tests(void) {
    HU_TEST_SUITE("Lifecycle");
    HU_RUN_TEST(test_migrate_invalid_args);
    HU_RUN_TEST(test_migrate_free_entries_null);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_migrate_read_nonexistent_path);
    HU_RUN_TEST(test_migrate_read_brain_db_with_temp);
#else
    HU_RUN_TEST(test_migrate_not_supported_without_sqlite);
#endif
    HU_RUN_TEST(test_cache_put_get);
    HU_RUN_TEST(test_cache_eviction);
    HU_RUN_TEST(test_cache_invalidate);
    HU_RUN_TEST(test_cache_clear);
    HU_RUN_TEST(test_hygiene_removes_oversized);
    HU_RUN_TEST(test_hygiene_removes_expired);
    HU_RUN_TEST(test_hygiene_deduplicates);
    HU_RUN_TEST(test_snapshot_export_import);
    HU_RUN_TEST(test_summarizer_truncation);
}
