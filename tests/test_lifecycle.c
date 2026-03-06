#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/lifecycle.h"
#include "seaclaw/memory/lifecycle/migrate.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define sc_mkdir(path) _mkdir(path)
#define close(fd)      _close(fd)
#else
#include <fcntl.h>
#include <unistd.h>
#define sc_mkdir(path) mkdir((path), 0755)
#endif

static void test_cache_put_get(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_cache_t *cache = sc_memory_cache_create(&alloc, 8);
    SC_ASSERT_NOT_NULL(cache);

    sc_memory_entry_t entry = {0};
    entry.key = "k1";
    entry.key_len = 2;
    entry.content = "content one";
    entry.content_len = 11;
    entry.category.tag = SC_MEMORY_CATEGORY_CORE;
    entry.timestamp = "2024-01-15T10:00:00Z";
    entry.timestamp_len = 20;

    sc_error_t err = sc_memory_cache_put(cache, "k1", 2, &entry);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(sc_memory_cache_count(cache), 1);

    sc_memory_entry_t out = {0};
    bool found = false;
    err = sc_memory_cache_get(cache, "k1", 2, &out, &found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(found);
    SC_ASSERT_EQ(out.key_len, 2);
    SC_ASSERT_EQ(memcmp(out.key, "k1", 2), 0);
    SC_ASSERT_EQ(out.content_len, 11);
    SC_ASSERT_EQ(memcmp(out.content, "content one", 11), 0);

    if (out.key)
        alloc.free(alloc.ctx, (void *)out.key, out.key_len + 1);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);
    if (out.timestamp)
        alloc.free(alloc.ctx, (void *)out.timestamp, out.timestamp_len + 1);

    sc_memory_cache_destroy(cache);
}

static void test_cache_eviction(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_cache_t *cache = sc_memory_cache_create(&alloc, 3);
    SC_ASSERT_NOT_NULL(cache);

    sc_memory_entry_t entry = {0};
    entry.category.tag = SC_MEMORY_CATEGORY_CORE;

    const char *keys[] = {"a", "b", "c", "d"};
    for (size_t i = 0; i < 4; i++) {
        entry.key = keys[i];
        entry.key_len = 1;
        entry.content = keys[i];
        entry.content_len = 1;
        sc_memory_cache_put(cache, keys[i], 1, &entry);
    }

    SC_ASSERT_EQ(sc_memory_cache_count(cache), 3);

    bool found = false;
    sc_memory_entry_t out = {0};
    sc_memory_cache_get(cache, "a", 1, &out, &found);
    SC_ASSERT_FALSE(found);

    sc_memory_cache_get(cache, "b", 1, &out, &found);
    SC_ASSERT_TRUE(found);
    if (out.key)
        alloc.free(alloc.ctx, (void *)out.key, out.key_len + 1);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);

    sc_memory_cache_get(cache, "c", 1, &out, &found);
    SC_ASSERT_TRUE(found);
    if (out.key)
        alloc.free(alloc.ctx, (void *)out.key, out.key_len + 1);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);

    sc_memory_cache_get(cache, "d", 1, &out, &found);
    SC_ASSERT_TRUE(found);
    if (out.key)
        alloc.free(alloc.ctx, (void *)out.key, out.key_len + 1);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);

    sc_memory_cache_destroy(cache);
}

static void test_cache_invalidate(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_cache_t *cache = sc_memory_cache_create(&alloc, 4);
    SC_ASSERT_NOT_NULL(cache);

    sc_memory_entry_t entry = {0};
    entry.category.tag = SC_MEMORY_CATEGORY_CORE;
    entry.key = "x";
    entry.key_len = 1;
    entry.content = "val";
    entry.content_len = 3;

    sc_memory_cache_put(cache, "x", 1, &entry);
    sc_memory_cache_put(cache, "y", 1, &entry);
    SC_ASSERT_EQ(sc_memory_cache_count(cache), 2);

    sc_memory_cache_invalidate(cache, "x", 1);
    SC_ASSERT_EQ(sc_memory_cache_count(cache), 1);

    bool found = false;
    sc_memory_entry_t out = {0};
    sc_memory_cache_get(cache, "x", 1, &out, &found);
    SC_ASSERT_FALSE(found);
    sc_memory_cache_get(cache, "y", 1, &out, &found);
    SC_ASSERT_TRUE(found);
    if (out.key)
        alloc.free(alloc.ctx, (void *)out.key, out.key_len + 1);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);

    sc_memory_cache_destroy(cache);
}

static void test_cache_clear(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_cache_t *cache = sc_memory_cache_create(&alloc, 4);
    SC_ASSERT_NOT_NULL(cache);

    sc_memory_entry_t entry = {0};
    entry.category.tag = SC_MEMORY_CATEGORY_CORE;
    entry.key = "k";
    entry.key_len = 1;
    entry.content = "v";
    entry.content_len = 1;

    sc_memory_cache_put(cache, "a", 1, &entry);
    sc_memory_cache_put(cache, "b", 1, &entry);
    SC_ASSERT_EQ(sc_memory_cache_count(cache), 2);

    sc_memory_cache_clear(cache);
    SC_ASSERT_EQ(sc_memory_cache_count(cache), 0);

    bool found = false;
    sc_memory_entry_t out = {0};
    sc_memory_cache_get(cache, "a", 1, &out, &found);
    SC_ASSERT_FALSE(found);

    sc_memory_cache_destroy(cache);
}

static void test_hygiene_removes_oversized(void) {
    sc_allocator_t alloc = sc_system_allocator();
#ifdef SC_ENABLE_SQLITE
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
#else
    sc_memory_t mem = sc_none_memory_create(&alloc);
#endif
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_CORE};
#ifdef SC_ENABLE_SQLITE
    char big[1025];
    memset(big, 'x', 1024);
    big[1024] = '\0';
    mem.vtable->store(mem.ctx, "big_key", 7, big, 1024, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "small", 5, "ok", 2, &cat, NULL, 0);
#endif

    sc_hygiene_config_t config = {
        .max_entries = 0,
        .max_entry_size = 512,
        .max_age_seconds = 0,
        .deduplicate = false,
    };
    sc_hygiene_stats_t stats = {0};
    sc_error_t err = sc_memory_hygiene_run(&alloc, &mem, &config, &stats);
    SC_ASSERT_EQ(err, SC_OK);

#ifdef SC_ENABLE_SQLITE
    SC_ASSERT_TRUE(stats.entries_scanned >= 1);
    SC_ASSERT_TRUE(stats.oversized_removed >= 1 || stats.entries_removed >= 1);
#endif

    mem.vtable->deinit(mem.ctx);
}

static void test_hygiene_removes_expired(void) {
    sc_allocator_t alloc = sc_system_allocator();
#ifdef SC_ENABLE_SQLITE
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
#else
    sc_memory_t mem = sc_none_memory_create(&alloc);
#endif
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_CORE};
#ifdef SC_ENABLE_SQLITE
    mem.vtable->store(mem.ctx, "old", 3, "old content", 11, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "new", 3, "new content", 11, &cat, NULL, 0);

    sc_hygiene_config_t config = {
        .max_entries = 0,
        .max_entry_size = 0,
        .max_age_seconds = 1,
        .deduplicate = false,
    };
    sc_hygiene_stats_t stats = {0};
    sc_error_t err = sc_memory_hygiene_run(&alloc, &mem, &config, &stats);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(stats.entries_scanned >= 1);

    sleep(2);
    sc_hygiene_stats_t stats2 = {0};
    err = sc_memory_hygiene_run(&alloc, &mem, &config, &stats2);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(stats2.expired_removed >= 1 || stats2.entries_removed >= 1);
#endif

    mem.vtable->deinit(mem.ctx);
}

static void test_hygiene_deduplicates(void) {
    sc_allocator_t alloc = sc_system_allocator();
#ifdef SC_ENABLE_SQLITE
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
#else
    sc_memory_t mem = sc_none_memory_create(&alloc);
#endif
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_CORE};
#ifdef SC_ENABLE_SQLITE
    mem.vtable->store(mem.ctx, "dup_a", 5, "same content", 12, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "dup_b", 5, "same content", 12, &cat, NULL, 0);
#endif

    sc_hygiene_config_t config = {
        .max_entries = 0,
        .max_entry_size = 0,
        .max_age_seconds = 0,
        .deduplicate = true,
    };
    sc_hygiene_stats_t stats = {0};
    sc_error_t err = sc_memory_hygiene_run(&alloc, &mem, &config, &stats);
    SC_ASSERT_EQ(err, SC_OK);

#ifdef SC_ENABLE_SQLITE
    SC_ASSERT_TRUE(stats.entries_scanned >= 1);
    if (stats.entries_scanned >= 2)
        SC_ASSERT_TRUE(stats.duplicates_removed >= 1 || stats.entries_removed >= 1);
#endif

    mem.vtable->deinit(mem.ctx);
}

static void test_snapshot_export_import(void) {
    sc_allocator_t alloc = sc_system_allocator();
#ifdef SC_ENABLE_SQLITE
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
#else
    sc_memory_t mem = sc_none_memory_create(&alloc);
#endif
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_CORE};
#ifdef SC_ENABLE_SQLITE
    mem.vtable->store(mem.ctx, "snap_key1", 9, "snap_val1", 9, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "snap_key2", 9, "snap_val2", 9, &cat, NULL, 0);
#endif

    char path_buf[] = "/tmp/seaclaw_snap_XXXXXX";
#ifdef _WIN32
    (void)path_buf;
    const char *path = "seaclaw_snap_test.json";
    size_t path_len = strlen(path);
#else
    int fd = mkstemp(path_buf);
    SC_ASSERT_TRUE(fd >= 0);
    close(fd);
    const char *path = path_buf;
    size_t path_len = strlen(path);
#endif

    sc_error_t err = sc_memory_snapshot_export(&alloc, &mem, path, path_len);
    SC_ASSERT_EQ(err, SC_OK);

#ifdef SC_ENABLE_SQLITE
    sc_memory_t mem2 = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem2.ctx);

    err = sc_memory_snapshot_import(&alloc, &mem2, path, path_len);
    SC_ASSERT_EQ(err, SC_OK);

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    err = mem2.vtable->list(mem2.ctx, &alloc, NULL, NULL, 0, &entries, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count >= 1);

    bool found_any = false;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].key_len == 9) {
            if (memcmp(entries[i].key, "snap_key1", 9) == 0 ||
                memcmp(entries[i].key, "snap_key2", 9) == 0)
                found_any = true;
        }
        sc_memory_entry_free_fields(&alloc, &entries[i]);
    }
    if (entries)
        alloc.free(alloc.ctx, entries, count * sizeof(sc_memory_entry_t));
    SC_ASSERT_TRUE(found_any);

    mem2.vtable->deinit(mem2.ctx);
#endif

#ifndef _WIN32
    unlink(path);
#endif
    mem.vtable->deinit(mem.ctx);
}

static void test_summarizer_truncation(void) {
    sc_allocator_t alloc = sc_system_allocator();
#ifdef SC_ENABLE_SQLITE
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
#else
    sc_memory_t mem = sc_none_memory_create(&alloc);
#endif
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_CORE};
#ifdef SC_ENABLE_SQLITE
    char long_content[256];
    memset(long_content, 'a', 255);
    long_content[255] = '\0';
    mem.vtable->store(mem.ctx, "long_entry", 10, long_content, 255, &cat, NULL, 0);
#endif

    sc_summarizer_config_t config = {
        .batch_size = 5,
        .max_summary_len = 50,
        .provider = NULL,
    };
    sc_summarizer_stats_t stats = {0};
    sc_error_t err = sc_memory_summarize(&alloc, &mem, &config, &stats);
    SC_ASSERT_EQ(err, SC_OK);

#ifdef SC_ENABLE_SQLITE
    SC_ASSERT_EQ(stats.entries_summarized, 1);
    SC_ASSERT_TRUE(stats.tokens_saved > 0);

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    err = mem.vtable->list(mem.ctx, &alloc, NULL, NULL, 0, &entries, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 1);
    SC_ASSERT_EQ(entries[0].content_len, 50);
    sc_memory_entry_free_fields(&alloc, &entries[0]);
    alloc.free(alloc.ctx, entries, sizeof(sc_memory_entry_t));
#endif

    mem.vtable->deinit(mem.ctx);
}

/* ─── Migrate: brain.db read and free ───────────────────────────────────────── */
static void test_migrate_invalid_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_sqlite_source_entry_t *out = NULL;
    size_t count = 0;
    sc_error_t err = sc_migrate_read_brain_db(NULL, "/tmp/x.db", &out, &count);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    err = sc_migrate_read_brain_db(&alloc, NULL, &out, &count);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_migrate_free_entries_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_migrate_free_entries(&alloc, NULL, 0);
    sc_migrate_free_entries(NULL, NULL, 0);
}

#ifdef SC_ENABLE_SQLITE
#include <sqlite3.h>

static void test_migrate_read_nonexistent_path(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_sqlite_source_entry_t *out = NULL;
    size_t count = 0;
    sc_error_t err =
        sc_migrate_read_brain_db(&alloc, "/tmp/seaclaw_nonexistent_migrate_12345.db", &out, &count);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_migrate_read_brain_db_with_temp(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char path_buf[] = "/tmp/seaclaw_migrate_XXXXXX";
#if defined(_WIN32) || defined(_WIN64)
    (void)path_buf;
    (void)alloc;
    return;
#else
    int fd = mkstemp(path_buf);
    SC_ASSERT_TRUE(fd >= 0);
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

    sc_sqlite_source_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = sc_migrate_read_brain_db(&alloc, path_buf, &entries, &count);
    unlink(path_buf);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(entries);
    SC_ASSERT_EQ(count, 1u);
    SC_ASSERT_NOT_NULL(entries[0].key);
    SC_ASSERT_NOT_NULL(entries[0].content);
    SC_ASSERT_STR_EQ(entries[0].key, "k1");
    SC_ASSERT_STR_EQ(entries[0].content, "content one");
    SC_ASSERT_EQ(entries[0].category ? strcmp(entries[0].category, "core") : 0, 0);

    sc_migrate_free_entries(&alloc, entries, count);
#endif
}
#else
static void test_migrate_not_supported_without_sqlite(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_sqlite_source_entry_t *out = NULL;
    size_t count = 0;
    sc_error_t err = sc_migrate_read_brain_db(&alloc, "/tmp/any.db", &out, &count);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
}
#endif

void run_lifecycle_tests(void) {
    SC_TEST_SUITE("Lifecycle");
    SC_RUN_TEST(test_migrate_invalid_args);
    SC_RUN_TEST(test_migrate_free_entries_null);
#ifdef SC_ENABLE_SQLITE
    SC_RUN_TEST(test_migrate_read_nonexistent_path);
    SC_RUN_TEST(test_migrate_read_brain_db_with_temp);
#else
    SC_RUN_TEST(test_migrate_not_supported_without_sqlite);
#endif
    SC_RUN_TEST(test_cache_put_get);
    SC_RUN_TEST(test_cache_eviction);
    SC_RUN_TEST(test_cache_invalidate);
    SC_RUN_TEST(test_cache_clear);
    SC_RUN_TEST(test_hygiene_removes_oversized);
    SC_RUN_TEST(test_hygiene_removes_expired);
    SC_RUN_TEST(test_hygiene_deduplicates);
    SC_RUN_TEST(test_snapshot_export_import);
    SC_RUN_TEST(test_summarizer_truncation);
}
