/* Comprehensive memory backend tests. */
#include "human/core/allocator.h"
#include "human/memory.h"
#include "test_framework.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_none_backend_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_STR_EQ(mem.vtable->name(mem.ctx), "none");
    mem.vtable->deinit(mem.ctx);
}

static void test_none_backend_store(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "k", 1, "v", 1, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_backend_recall_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    hu_error_t err = mem.vtable->recall(mem.ctx, &alloc, "q", 1, 10, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(count, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_backend_empty_strings(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "", 0, "", 0, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    mem.vtable->deinit(mem.ctx);
}

#ifdef HU_ENABLE_SQLITE
static void test_sqlite_store_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err =
        mem.vtable->store(mem.ctx, "user_pref", 9, "likes dark mode", 15, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "dark", 4, 5, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(out[0].key, "user_pref");
    if (out) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(&alloc, &out[i]);
        alloc.free(alloc.ctx, out, count * sizeof(hu_memory_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_get_forget_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "getme", 5, "value", 5, &cat, NULL, 0);

    hu_memory_entry_t entry = {0};
    bool found = false;
    hu_error_t err = mem.vtable->get(mem.ctx, &alloc, "getme", 5, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_STR_EQ(entry.key, "getme");
    hu_memory_entry_free_fields(&alloc, &entry);

    bool deleted = false;
    err = mem.vtable->forget(mem.ctx, "getme", 5, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(deleted);

    memset(&entry, 0, sizeof(entry));
    found = false;
    mem.vtable->get(mem.ctx, &alloc, "getme", 5, &entry, &found);
    HU_ASSERT_FALSE(found);

    size_t n = 0;
    err = mem.vtable->count(mem.ctx, &n);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(n, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_session_store(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_session_store_t ss = hu_sqlite_memory_get_session_store(&mem);
    HU_ASSERT_NOT_NULL(ss.ctx);

    const char *sid = "sess123";
    hu_error_t err = ss.vtable->save_message(ss.ctx, sid, 7, "user", 4, "hello", 5);
    HU_ASSERT_EQ(err, HU_OK);

    hu_message_entry_t *msgs = NULL;
    size_t count = 0;
    err = ss.vtable->load_messages(ss.ctx, &alloc, sid, 7, &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(count >= 1);
    if (msgs) {
        for (size_t i = 0; i < count; i++) {
            if (msgs[i].role)
                alloc.free(alloc.ctx, (void *)msgs[i].role, msgs[i].role_len + 1);
            if (msgs[i].content)
                alloc.free(alloc.ctx, (void *)msgs[i].content, msgs[i].content_len + 1);
        }
        alloc.free(alloc.ctx, msgs, count * sizeof(hu_message_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "a", 1, "x", 1, &cat, NULL, 0);

    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    hu_error_t err = mem.vtable->list(mem.ctx, &alloc, NULL, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(count >= 1);
    if (out) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(&alloc, &out[i]);
        alloc.free(alloc.ctx, out, count * sizeof(hu_memory_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}

/* Error-path tests for SQLite memory engine */
static void test_sqlite_create_null_path_uses_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_NOT_NULL(mem.vtable);
    HU_ASSERT_STR_EQ(mem.vtable->name(mem.ctx), "sqlite");
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_create_invalid_path_fails_gracefully(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, "/nonexistent_dir/test.db");
    HU_ASSERT_NULL(mem.ctx);
    HU_ASSERT_NULL(mem.vtable);
}

static void test_sqlite_store_null_key_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, NULL, 0, "content", 7, &cat, NULL, 0);
    HU_ASSERT_NEQ(err, HU_OK);
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_store_empty_key_handled(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "", 0, "content", 7, &cat, NULL, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_store_null_content_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "key", 3, NULL, 0, &cat, NULL, 0);
    HU_ASSERT_NEQ(err, HU_OK);
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_recall_null_query_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_entry_t *out = (hu_memory_entry_t *)0xdeadbeef;
    size_t count = 99;
    hu_error_t err = mem.vtable->recall(mem.ctx, &alloc, NULL, 0, 10, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_forget_null_key_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    bool deleted = false;
    hu_error_t err = mem.vtable->forget(mem.ctx, NULL, 0, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(deleted);
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_get_nonexistent_key_returns_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_entry_t entry = {0};
    bool found = true;
    hu_error_t err = mem.vtable->get(mem.ctx, &alloc, "nonexistent_key_xyz", 19, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(found);
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_recall_nonexistent_query_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    hu_error_t err = mem.vtable->recall(mem.ctx, &alloc, "xyznonexistentquery123", 21, 10, NULL, 0,
                                        &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    if (out) {
        alloc.free(alloc.ctx, out, 10 * sizeof(hu_memory_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_store_recall_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    const char *key = "roundtrip_key";
    const char *content = "roundtrip content value";
    hu_error_t err = mem.vtable->store(mem.ctx, key, 13, content, 23, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "roundtrip", 9, 5, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(out[0].key, key);
    HU_ASSERT_STR_EQ(out[0].content, content);
    if (out) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(&alloc, &out[i]);
        alloc.free(alloc.ctx, out, count * sizeof(hu_memory_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_forget_then_recall_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    const char *key = "forget_then_recall";
    mem.vtable->store(mem.ctx, key, 18, "content", 7, &cat, NULL, 0);

    bool deleted = false;
    hu_error_t err = mem.vtable->forget(mem.ctx, key, 18, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(deleted);

    hu_memory_entry_t entry = {0};
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, key, 18, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(found);

    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "forget_then_recall", 17, 5, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    if (out) {
        alloc.free(alloc.ctx, out, 5 * sizeof(hu_memory_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}
#endif

#ifdef HU_HAS_MARKDOWN_ENGINE
static void test_markdown_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_markdown_memory_create(&alloc, "/tmp/human_md_test");
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_STR_EQ(mem.vtable->name(mem.ctx), "markdown");
    mem.vtable->deinit(mem.ctx);
}

static void test_markdown_store_get_list_forget(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char tmp[] = "/tmp/human_md_lifecycle_XXXXXX";
    char *dir = mkdtemp(tmp);
    HU_ASSERT_NOT_NULL(dir);
    hu_memory_t mem = hu_markdown_memory_create(&alloc, dir);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "k1", 2, "content one", 11, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t entry = {0};
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "k1", 2, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(found);
    hu_memory_entry_free_fields(&alloc, &entry);

    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    err = mem.vtable->list(mem.ctx, &alloc, &cat, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(count >= 1);
    if (out) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(&alloc, &out[i]);
        alloc.free(alloc.ctx, out, count * sizeof(hu_memory_entry_t));
    }

    bool deleted = false;
    err = mem.vtable->forget(mem.ctx, "k1", 2, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(deleted);

    mem.vtable->deinit(mem.ctx);
    rmdir(dir);
}
#endif

static void test_none_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    HU_ASSERT_TRUE(mem.vtable->health_check(mem.ctx));
    mem.vtable->deinit(mem.ctx);
}

static void test_none_recall_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    hu_error_t err = mem.vtable->recall(mem.ctx, &alloc, "anything", 8, 5, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(count, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_forget_nonexistent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    bool deleted = false;
    hu_error_t err = mem.vtable->forget(mem.ctx, "nonexistent", 10, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(deleted);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_get_nonexistent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_memory_entry_t entry = {0};
    bool found = false;
    hu_error_t err = mem.vtable->get(mem.ctx, &alloc, "missing", 7, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(found);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_count_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    size_t n = 1;
    hu_error_t err = mem.vtable->count(mem.ctx, &n);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(n, 0u);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_store_multiple(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "a", 1, "1", 1, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    err = mem.vtable->store(mem.ctx, "b", 1, "2", 1, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_list_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->list(mem.ctx, &alloc, &cat, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(count, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_store_with_session(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CONVERSATION};
    const char *sid = "sess-abc";
    hu_error_t err = mem.vtable->store(mem.ctx, "conv_k", 6, "conv content", 12, &cat, sid, 7);
    HU_ASSERT_EQ(err, HU_OK);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_categories_core_daily_conversation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_memory_category_t core = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_memory_category_t daily = {.tag = HU_MEMORY_CATEGORY_DAILY};
    hu_memory_category_t conv = {.tag = HU_MEMORY_CATEGORY_CONVERSATION};
    mem.vtable->store(mem.ctx, "c", 1, "core", 4, &core, NULL, 0);
    mem.vtable->store(mem.ctx, "d", 1, "daily", 5, &daily, NULL, 0);
    mem.vtable->store(mem.ctx, "v", 1, "conv", 4, &conv, NULL, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_recall_limit_respected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_memory_entry_t *out = NULL;
    size_t count = 99;
    hu_error_t err = mem.vtable->recall(mem.ctx, &alloc, "q", 1, 10, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_NULL(out);
    mem.vtable->deinit(mem.ctx);
}

void run_memory_full_tests(void) {
    HU_TEST_SUITE("Memory full - None backend");
    HU_RUN_TEST(test_none_backend_create);
    HU_RUN_TEST(test_none_backend_store);
    HU_RUN_TEST(test_none_backend_recall_empty);
    HU_RUN_TEST(test_none_backend_empty_strings);
    HU_RUN_TEST(test_none_health_check);
    HU_RUN_TEST(test_none_recall_returns_empty);
    HU_RUN_TEST(test_none_forget_nonexistent);
    HU_RUN_TEST(test_none_get_nonexistent);
    HU_RUN_TEST(test_none_count_zero);
    HU_RUN_TEST(test_none_store_multiple);
    HU_RUN_TEST(test_none_list_empty);
    HU_RUN_TEST(test_none_store_with_session);
    HU_RUN_TEST(test_none_categories_core_daily_conversation);
    HU_RUN_TEST(test_none_recall_limit_respected);

#ifdef HU_ENABLE_SQLITE
    HU_TEST_SUITE("Memory full - SQLite");
    HU_RUN_TEST(test_sqlite_store_recall);
    HU_RUN_TEST(test_sqlite_get_forget_count);
    HU_RUN_TEST(test_sqlite_session_store);
    HU_RUN_TEST(test_sqlite_list);
    HU_RUN_TEST(test_sqlite_create_null_path_uses_memory);
    HU_RUN_TEST(test_sqlite_create_invalid_path_fails_gracefully);
    HU_RUN_TEST(test_sqlite_store_null_key_fails);
    HU_RUN_TEST(test_sqlite_store_empty_key_handled);
    HU_RUN_TEST(test_sqlite_store_null_content_fails);
    HU_RUN_TEST(test_sqlite_recall_null_query_returns_empty);
    HU_RUN_TEST(test_sqlite_forget_null_key_succeeds);
    HU_RUN_TEST(test_sqlite_get_nonexistent_key_returns_not_found);
    HU_RUN_TEST(test_sqlite_recall_nonexistent_query_returns_empty);
    HU_RUN_TEST(test_sqlite_store_recall_roundtrip);
    HU_RUN_TEST(test_sqlite_forget_then_recall_returns_empty);
#endif

#ifdef HU_HAS_MARKDOWN_ENGINE
    HU_TEST_SUITE("Memory full - Markdown");
    HU_RUN_TEST(test_markdown_create);
    HU_RUN_TEST(test_markdown_store_get_list_forget);
#endif
}
