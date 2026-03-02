/* Comprehensive memory backend tests. */
#include "test_framework.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdlib.h>

static void test_none_backend_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    SC_ASSERT_NOT_NULL(mem.ctx);
    SC_ASSERT_STR_EQ(mem.vtable->name(mem.ctx), "none");
    mem.vtable->deinit(mem.ctx);
}

static void test_none_backend_store(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    sc_error_t err = mem.vtable->store(mem.ctx, "k", 1, "v", 1, &cat, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_backend_recall_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_memory_entry_t *out = NULL;
    size_t count = 0;
    sc_error_t err = mem.vtable->recall(mem.ctx, &alloc, "q", 1, 10, NULL, 0, &out, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NULL(out);
    SC_ASSERT_EQ(count, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_backend_empty_strings(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    sc_error_t err = mem.vtable->store(mem.ctx, "", 0, "", 0, &cat, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    mem.vtable->deinit(mem.ctx);
}

#ifdef SC_ENABLE_SQLITE
static void test_sqlite_store_recall(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    sc_error_t err = mem.vtable->store(mem.ctx, "user_pref", 9, "likes dark mode", 15, &cat, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);

    sc_memory_entry_t *out = NULL;
    size_t count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "dark", 4, 5, NULL, 0, &out, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_EQ(count, 1);
    SC_ASSERT_STR_EQ(out[0].key, "user_pref");
    if (out) {
        for (size_t i = 0; i < count; i++) {
            if (out[i].id) alloc.free(alloc.ctx, (void *)out[i].id, out[i].id_len + 1);
            if (out[i].key) alloc.free(alloc.ctx, (void *)out[i].key, out[i].key_len + 1);
            if (out[i].content) alloc.free(alloc.ctx, (void *)out[i].content, out[i].content_len + 1);
            if (out[i].category.data.custom.name) alloc.free(alloc.ctx,
                (void *)out[i].category.data.custom.name, out[i].category.data.custom.name_len + 1);
            if (out[i].timestamp) alloc.free(alloc.ctx, (void *)out[i].timestamp, out[i].timestamp_len + 1);
            if (out[i].session_id) alloc.free(alloc.ctx, (void *)out[i].session_id, out[i].session_id_len + 1);
        }
        alloc.free(alloc.ctx, out, count * sizeof(sc_memory_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_get_forget_count(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "getme", 5, "value", 5, &cat, NULL, 0);

    sc_memory_entry_t entry = {0};
    bool found = false;
    sc_error_t err = mem.vtable->get(mem.ctx, &alloc, "getme", 5, &entry, &found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(found);
    SC_ASSERT_STR_EQ(entry.key, "getme");

    bool deleted = false;
    err = mem.vtable->forget(mem.ctx, "getme", 5, &deleted);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(deleted);

    found = false;
    mem.vtable->get(mem.ctx, &alloc, "getme", 5, &entry, &found);
    SC_ASSERT_FALSE(found);

    size_t n = 0;
    err = mem.vtable->count(mem.ctx, &n);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(n, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_session_store(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    sc_session_store_t ss = sc_sqlite_memory_get_session_store(&mem);
    SC_ASSERT_NOT_NULL(ss.ctx);

    const char *sid = "sess123";
    sc_error_t err = ss.vtable->save_message(ss.ctx, sid, 7, "user", 4, "hello", 5);
    SC_ASSERT_EQ(err, SC_OK);

    sc_message_entry_t *msgs = NULL;
    size_t count = 0;
    err = ss.vtable->load_messages(ss.ctx, &alloc, sid, 7, &msgs, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(count >= 1);
    if (msgs) {
        for (size_t i = 0; i < count; i++) {
            if (msgs[i].role) alloc.free(alloc.ctx, (void *)msgs[i].role, msgs[i].role_len + 1);
            if (msgs[i].content) alloc.free(alloc.ctx, (void *)msgs[i].content, msgs[i].content_len + 1);
        }
        alloc.free(alloc.ctx, msgs, count * sizeof(sc_message_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}

static void test_sqlite_list(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "a", 1, "x", 1, &cat, NULL, 0);

    sc_memory_entry_t *out = NULL;
    size_t count = 0;
    sc_error_t err = mem.vtable->list(mem.ctx, &alloc, NULL, NULL, 0, &out, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(count >= 1);
    if (out) {
        for (size_t i = 0; i < count; i++) {
            if (out[i].id) alloc.free(alloc.ctx, (void *)out[i].id, out[i].id_len + 1);
            if (out[i].key) alloc.free(alloc.ctx, (void *)out[i].key, out[i].key_len + 1);
            if (out[i].content) alloc.free(alloc.ctx, (void *)out[i].content, out[i].content_len + 1);
            if (out[i].category.data.custom.name) alloc.free(alloc.ctx,
                (void *)out[i].category.data.custom.name, out[i].category.data.custom.name_len + 1);
            if (out[i].timestamp) alloc.free(alloc.ctx, (void *)out[i].timestamp, out[i].timestamp_len + 1);
            if (out[i].session_id) alloc.free(alloc.ctx, (void *)out[i].session_id, out[i].session_id_len + 1);
        }
        alloc.free(alloc.ctx, out, count * sizeof(sc_memory_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}
#endif

#ifdef SC_HAS_MARKDOWN_ENGINE
static void test_markdown_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_markdown_memory_create(&alloc, "/tmp/seaclaw_md_test");
    SC_ASSERT_NOT_NULL(mem.ctx);
    SC_ASSERT_STR_EQ(mem.vtable->name(mem.ctx), "markdown");
    mem.vtable->deinit(mem.ctx);
}

static void test_markdown_store_get_list_forget(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char tmp[] = "/tmp/seaclaw_md_lifecycle_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);
    sc_memory_t mem = sc_markdown_memory_create(&alloc, dir);
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    sc_error_t err = mem.vtable->store(mem.ctx, "k1", 2, "content one", 11, &cat, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);

    sc_memory_entry_t entry = {0};
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "k1", 2, &entry, &found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(found);
    sc_memory_entry_free_fields(&alloc, &entry);

    sc_memory_entry_t *out = NULL;
    size_t count = 0;
    err = mem.vtable->list(mem.ctx, &alloc, &cat, NULL, 0, &out, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(count >= 1);
    if (out) {
        for (size_t i = 0; i < count; i++) sc_memory_entry_free_fields(&alloc, &out[i]);
        alloc.free(alloc.ctx, out, count * sizeof(sc_memory_entry_t));
    }

    bool deleted = false;
    err = mem.vtable->forget(mem.ctx, "k1", 2, &deleted);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(deleted);

    mem.vtable->deinit(mem.ctx);
    rmdir(dir);
}
#endif

static void test_none_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    SC_ASSERT_TRUE(mem.vtable->health_check(mem.ctx));
    mem.vtable->deinit(mem.ctx);
}

static void test_none_recall_returns_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_memory_entry_t *out = NULL;
    size_t count = 0;
    sc_error_t err = mem.vtable->recall(mem.ctx, &alloc, "anything", 8, 5, NULL, 0, &out, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NULL(out);
    SC_ASSERT_EQ(count, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_forget_nonexistent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    bool deleted = false;
    sc_error_t err = mem.vtable->forget(mem.ctx, "nonexistent", 10, &deleted);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_FALSE(deleted);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_get_nonexistent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_memory_entry_t entry = {0};
    bool found = false;
    sc_error_t err = mem.vtable->get(mem.ctx, &alloc, "missing", 7, &entry, &found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_FALSE(found);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_count_zero(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    size_t n = 1;
    sc_error_t err = mem.vtable->count(mem.ctx, &n);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(n, 0u);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_store_multiple(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    sc_error_t err = mem.vtable->store(mem.ctx, "a", 1, "1", 1, &cat, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    err = mem.vtable->store(mem.ctx, "b", 1, "2", 1, &cat, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_list_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_memory_entry_t *out = NULL;
    size_t count = 0;
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    sc_error_t err = mem.vtable->list(mem.ctx, &alloc, &cat, NULL, 0, &out, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NULL(out);
    SC_ASSERT_EQ(count, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_store_with_session(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CONVERSATION };
    const char *sid = "sess-abc";
    sc_error_t err = mem.vtable->store(mem.ctx, "conv_k", 6, "conv content", 12, &cat, sid, 7);
    SC_ASSERT_EQ(err, SC_OK);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_categories_core_daily_conversation(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_memory_category_t core = { .tag = SC_MEMORY_CATEGORY_CORE };
    sc_memory_category_t daily = { .tag = SC_MEMORY_CATEGORY_DAILY };
    sc_memory_category_t conv = { .tag = SC_MEMORY_CATEGORY_CONVERSATION };
    mem.vtable->store(mem.ctx, "c", 1, "core", 4, &core, NULL, 0);
    mem.vtable->store(mem.ctx, "d", 1, "daily", 5, &daily, NULL, 0);
    mem.vtable->store(mem.ctx, "v", 1, "conv", 4, &conv, NULL, 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_none_recall_limit_respected(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_memory_entry_t *out = NULL;
    size_t count = 99;
    sc_error_t err = mem.vtable->recall(mem.ctx, &alloc, "q", 1, 10, NULL, 0, &out, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0);
    SC_ASSERT_NULL(out);
    mem.vtable->deinit(mem.ctx);
}

void run_memory_full_tests(void) {
    SC_TEST_SUITE("Memory full - None backend");
    SC_RUN_TEST(test_none_backend_create);
    SC_RUN_TEST(test_none_backend_store);
    SC_RUN_TEST(test_none_backend_recall_empty);
    SC_RUN_TEST(test_none_backend_empty_strings);
    SC_RUN_TEST(test_none_health_check);
    SC_RUN_TEST(test_none_recall_returns_empty);
    SC_RUN_TEST(test_none_forget_nonexistent);
    SC_RUN_TEST(test_none_get_nonexistent);
    SC_RUN_TEST(test_none_count_zero);
    SC_RUN_TEST(test_none_store_multiple);
    SC_RUN_TEST(test_none_list_empty);
    SC_RUN_TEST(test_none_store_with_session);
    SC_RUN_TEST(test_none_categories_core_daily_conversation);
    SC_RUN_TEST(test_none_recall_limit_respected);

#ifdef SC_ENABLE_SQLITE
    SC_TEST_SUITE("Memory full - SQLite");
    SC_RUN_TEST(test_sqlite_store_recall);
    SC_RUN_TEST(test_sqlite_get_forget_count);
    SC_RUN_TEST(test_sqlite_session_store);
    SC_RUN_TEST(test_sqlite_list);
#endif

#ifdef SC_HAS_MARKDOWN_ENGINE
    SC_TEST_SUITE("Memory full - Markdown");
    SC_RUN_TEST(test_markdown_create);
    SC_RUN_TEST(test_markdown_store_get_list_forget);
#endif
}
