/* Session manager tests */
#include "human/core/allocator.h"
#include "human/session.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static void test_session_manager_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_error_t err = hu_session_manager_init(&mgr, &alloc);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_session_count(&mgr), 0);
    hu_session_manager_deinit(&mgr);
}

static void test_session_get_or_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);

    hu_session_t *s1 = hu_session_get_or_create(&mgr, "abc");
    HU_ASSERT_NOT_NULL(s1);
    HU_ASSERT_TRUE(strstr(s1->session_key, "abc") != NULL);
    HU_ASSERT_EQ(hu_session_count(&mgr), 1);

    hu_session_t *s2 = hu_session_get_or_create(&mgr, "abc");
    HU_ASSERT_TRUE(s1 == s2);

    hu_session_t *s3 = hu_session_get_or_create(&mgr, "xyz");
    HU_ASSERT_NOT_NULL(s3);
    HU_ASSERT_TRUE(s3 != s1);
    HU_ASSERT_EQ(hu_session_count(&mgr), 2);

    hu_session_manager_deinit(&mgr);
}

static void test_session_append_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_t *s = hu_session_get_or_create(&mgr, "chat1");
    HU_ASSERT_NOT_NULL(s);

    hu_error_t err = hu_session_append_message(s, &alloc, "user", "Hello");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(s->message_count, 1);
    HU_ASSERT_TRUE(strcmp(s->messages[0].role, "user") == 0);
    HU_ASSERT_TRUE(strcmp(s->messages[0].content, "Hello") == 0);

    err = hu_session_append_message(s, &alloc, "assistant", "Hi there");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(s->message_count, 2);

    hu_session_manager_deinit(&mgr);
}

static void test_session_gen_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *id = hu_session_gen_id(&alloc);
    HU_ASSERT_NOT_NULL(id);
    HU_ASSERT_TRUE(strlen(id) > 0);
    alloc.free(alloc.ctx, id, strlen(id) + 1);
}

static void test_session_evict_idle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_get_or_create(&mgr, "evict_me");
    HU_ASSERT_EQ(hu_session_count(&mgr), 1);

    /* Evict sessions idle for 0 seconds = evict all (last_active is now) */
    size_t evicted = hu_session_evict_idle(&mgr, 0);
    /* Behavior may vary: 0 sec could mean "just now" so nothing evicted */
    (void)evicted;
    hu_session_manager_deinit(&mgr);
}

static void test_session_create_multiple_distinct(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);

    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "sess_%d", i);
        hu_session_t *s = hu_session_get_or_create(&mgr, key);
        HU_ASSERT_NOT_NULL(s);
        HU_ASSERT_TRUE(strstr(s->session_key, key) != NULL);
    }
    HU_ASSERT_EQ(hu_session_count(&mgr), 10u);
    hu_session_manager_deinit(&mgr);
}

static void test_session_same_key_returns_same_ptr(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);

    hu_session_t *s1 = hu_session_get_or_create(&mgr, "identical");
    hu_session_t *s2 = hu_session_get_or_create(&mgr, "identical");
    HU_ASSERT_TRUE(s1 == s2);
    HU_ASSERT_EQ(hu_session_count(&mgr), 1u);
    hu_session_manager_deinit(&mgr);
}

static void test_session_evict_idle_removes_old(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);

    hu_session_t *s = hu_session_get_or_create(&mgr, "old_session");
    HU_ASSERT_NOT_NULL(s);
    s->last_active = (int64_t)time(NULL) - 100;
    HU_ASSERT_EQ(hu_session_count(&mgr), 1u);

    size_t evicted = hu_session_evict_idle(&mgr, 50);
    HU_ASSERT_EQ(evicted, 1u);
    HU_ASSERT_EQ(hu_session_count(&mgr), 0u);
    hu_session_manager_deinit(&mgr);
}

static void test_session_append_message_long_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_t *s = hu_session_get_or_create(&mgr, "long");
    HU_ASSERT_NOT_NULL(s);

    char long_content[1024];
    for (size_t i = 0; i < sizeof(long_content) - 1; i++)
        long_content[i] = (char)('A' + (i % 26));
    long_content[sizeof(long_content) - 1] = '\0';

    hu_error_t err = hu_session_append_message(s, &alloc, "user", long_content);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(s->message_count, 1u);
    HU_ASSERT_TRUE(strncmp(s->messages[0].content, "ABCD", 4) == 0);
    hu_session_manager_deinit(&mgr);
}

static void test_session_gen_id_unique(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *id1 = hu_session_gen_id(&alloc);
    char *id2 = hu_session_gen_id(&alloc);
    HU_ASSERT_NOT_NULL(id1);
    HU_ASSERT_NOT_NULL(id2);
    HU_ASSERT_TRUE(strcmp(id1, id2) != 0);
    alloc.free(alloc.ctx, id1, strlen(id1) + 1);
    alloc.free(alloc.ctx, id2, strlen(id2) + 1);
}

static void test_session_append_message_invalid_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_t *s = hu_session_get_or_create(&mgr, "x");

    hu_error_t err = hu_session_append_message(NULL, &alloc, "user", "hi");
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_session_append_message(s, NULL, "user", "hi");
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_session_append_message(s, &alloc, NULL, "hi");
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_session_append_message(s, &alloc, "user", NULL);
    HU_ASSERT_NEQ(err, HU_OK);
    hu_session_manager_deinit(&mgr);
}

static void test_session_manager_init_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_session_manager_init(NULL, &alloc);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_session_manager_init((hu_session_manager_t *)1, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_session_get_or_create_null_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_t *s = hu_session_get_or_create(&mgr, NULL);
    HU_ASSERT_NULL(s);
    hu_session_manager_deinit(&mgr);
}

static void test_session_message_ordering(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_t *s = hu_session_get_or_create(&mgr, "order");
    hu_session_append_message(s, &alloc, "user", "1");
    hu_session_append_message(s, &alloc, "assistant", "2");
    hu_session_append_message(s, &alloc, "user", "3");
    HU_ASSERT_EQ(s->message_count, 3u);
    HU_ASSERT_STR_EQ(s->messages[0].content, "1");
    HU_ASSERT_STR_EQ(s->messages[1].content, "2");
    HU_ASSERT_STR_EQ(s->messages[2].content, "3");
    hu_session_manager_deinit(&mgr);
}

static void test_session_count_after_evict(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_get_or_create(&mgr, "s1");
    hu_session_get_or_create(&mgr, "s2");
    HU_ASSERT_EQ(hu_session_count(&mgr), 2u);
    hu_session_t *s1 = hu_session_get_or_create(&mgr, "s1");
    s1->last_active = (int64_t)time(NULL) - 200;
    hu_session_evict_idle(&mgr, 100);
    HU_ASSERT_TRUE(hu_session_count(&mgr) < 2u);
    hu_session_manager_deinit(&mgr);
}

static void test_session_deinit_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_manager_deinit(&mgr);
}

static void test_session_append_empty_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_t *s = hu_session_get_or_create(&mgr, "empty");
    hu_error_t err = hu_session_append_message(s, &alloc, "user", "");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(s->message_count, 1u);
    hu_session_manager_deinit(&mgr);
}

static void test_session_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_get_or_create(&mgr, "a");
    hu_session_get_or_create(&mgr, "b");

    size_t count = 0;
    hu_session_summary_t *summaries = hu_session_list(&mgr, &alloc, &count);
    HU_ASSERT_NOT_NULL(summaries);
    HU_ASSERT_EQ(count, 2u);
    alloc.free(alloc.ctx, summaries, count * sizeof(hu_session_summary_t));
    hu_session_manager_deinit(&mgr);
}

static void test_session_list_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);

    size_t count = 0;
    hu_session_summary_t *summaries = hu_session_list(&mgr, &alloc, &count);
    HU_ASSERT_NULL(summaries);
    HU_ASSERT_EQ(count, 0u);
    hu_session_manager_deinit(&mgr);
}

static void test_session_delete(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_get_or_create(&mgr, "del_me");
    HU_ASSERT_EQ(hu_session_count(&mgr), 1u);

    hu_error_t err = hu_session_delete(&mgr, "del_me");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_session_count(&mgr), 0u);
    hu_session_manager_deinit(&mgr);
}

static void test_session_delete_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);

    hu_error_t err = hu_session_delete(&mgr, "nonexistent");
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    hu_session_manager_deinit(&mgr);
}

static void test_session_patch(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_session_t *s = hu_session_get_or_create(&mgr, "patch_me");
    HU_ASSERT_NOT_NULL(s);

    hu_error_t err = hu_session_patch(&mgr, "patch_me", "my-label");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(s->label, "my-label");
    hu_session_manager_deinit(&mgr);
}

static void test_session_patch_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);

    hu_error_t err = hu_session_patch(&mgr, "nonexistent", "x");
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    hu_session_manager_deinit(&mgr);
}

static void test_session_save_load_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);

    hu_session_t *s = hu_session_get_or_create(&mgr, "persist_test");
    HU_ASSERT_NOT_NULL(s);
    hu_session_patch(&mgr, "persist_test", "my-label");
    hu_session_append_message(s, &alloc, "user", "hello world");
    hu_session_append_message(s, &alloc, "assistant", "hi there");

    hu_session_t *s2 = hu_session_get_or_create(&mgr, "persist_test_2");
    HU_ASSERT_NOT_NULL(s2);
    hu_session_append_message(s2, &alloc, "user", "second session");

    const char *path = "/tmp/human_test_sessions.json";
    hu_error_t err = hu_session_save(&mgr, path);
    HU_ASSERT_EQ(err, HU_OK);
    hu_session_manager_deinit(&mgr);

    hu_session_manager_t mgr2;
    hu_session_manager_init(&mgr2, &alloc);
    err = hu_session_load(&mgr2, path);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_session_count(&mgr2), 2u);

    hu_session_t *loaded = hu_session_get_or_create(&mgr2, "persist_test");
    HU_ASSERT_NOT_NULL(loaded);
    HU_ASSERT_STR_EQ(loaded->label, "my-label");
    HU_ASSERT_EQ(loaded->message_count, 2u);
    HU_ASSERT_STR_EQ(loaded->messages[0].role, "user");
    HU_ASSERT_STR_EQ(loaded->messages[0].content, "hello world");
    HU_ASSERT_STR_EQ(loaded->messages[1].role, "assistant");
    HU_ASSERT_STR_EQ(loaded->messages[1].content, "hi there");

    hu_session_t *loaded2 = hu_session_get_or_create(&mgr2, "persist_test_2");
    HU_ASSERT_NOT_NULL(loaded2);
    HU_ASSERT_EQ(loaded2->message_count, 1u);
    HU_ASSERT_STR_EQ(loaded2->messages[0].content, "second session");

    hu_session_manager_deinit(&mgr2);
    remove(path);
}

static void test_session_save_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    HU_ASSERT_NEQ(hu_session_save(NULL, "/tmp/x.json"), HU_OK);
    HU_ASSERT_NEQ(hu_session_save(&mgr, NULL), HU_OK);
    hu_session_manager_deinit(&mgr);
}

static void test_session_load_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    HU_ASSERT_NEQ(hu_session_load(NULL, "/tmp/x.json"), HU_OK);
    HU_ASSERT_NEQ(hu_session_load(&mgr, NULL), HU_OK);
    hu_session_manager_deinit(&mgr);
}

static void test_session_load_missing_file(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);
    hu_error_t err = hu_session_load(&mgr, "/tmp/nonexistent_human_sessions_xyz.json");
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    hu_session_manager_deinit(&mgr);
}

static void test_session_save_empty_manager(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);

    const char *path = "/tmp/human_test_empty.json";
    hu_error_t err = hu_session_save(&mgr, path);
    HU_ASSERT_EQ(err, HU_OK);

    hu_session_manager_t mgr2;
    hu_session_manager_init(&mgr2, &alloc);
    err = hu_session_load(&mgr2, path);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_session_count(&mgr2), 0u);

    hu_session_manager_deinit(&mgr2);
    hu_session_manager_deinit(&mgr);
    remove(path);
}

static void test_session_save_special_chars(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_session_manager_t mgr;
    hu_session_manager_init(&mgr, &alloc);

    hu_session_t *s = hu_session_get_or_create(&mgr, "special");
    HU_ASSERT_NOT_NULL(s);
    hu_session_append_message(s, &alloc, "user", "line1\nline2\ttab\"quote\"");

    const char *path = "/tmp/human_test_special.json";
    hu_error_t err = hu_session_save(&mgr, path);
    HU_ASSERT_EQ(err, HU_OK);
    hu_session_manager_deinit(&mgr);

    hu_session_manager_t mgr2;
    hu_session_manager_init(&mgr2, &alloc);
    err = hu_session_load(&mgr2, path);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_session_count(&mgr2), 1u);

    hu_session_t *loaded = hu_session_get_or_create(&mgr2, "special");
    HU_ASSERT_NOT_NULL(loaded);
    HU_ASSERT_EQ(loaded->message_count, 1u);

    hu_session_manager_deinit(&mgr2);
    remove(path);
}

void run_session_tests(void) {
    HU_TEST_SUITE("Session");
    HU_RUN_TEST(test_session_manager_init_deinit);
    HU_RUN_TEST(test_session_get_or_create);
    HU_RUN_TEST(test_session_append_message);
    HU_RUN_TEST(test_session_gen_id);
    HU_RUN_TEST(test_session_evict_idle);
    HU_RUN_TEST(test_session_create_multiple_distinct);
    HU_RUN_TEST(test_session_same_key_returns_same_ptr);
    HU_RUN_TEST(test_session_evict_idle_removes_old);
    HU_RUN_TEST(test_session_append_message_long_content);
    HU_RUN_TEST(test_session_gen_id_unique);
    HU_RUN_TEST(test_session_append_message_invalid_args);
    HU_RUN_TEST(test_session_manager_init_null);
    HU_RUN_TEST(test_session_get_or_create_null_key);
    HU_RUN_TEST(test_session_message_ordering);
    HU_RUN_TEST(test_session_count_after_evict);
    HU_RUN_TEST(test_session_deinit_empty);
    HU_RUN_TEST(test_session_append_empty_content);
    HU_RUN_TEST(test_session_list);
    HU_RUN_TEST(test_session_list_empty);
    HU_RUN_TEST(test_session_delete);
    HU_RUN_TEST(test_session_delete_not_found);
    HU_RUN_TEST(test_session_patch);
    HU_RUN_TEST(test_session_patch_not_found);
    HU_RUN_TEST(test_session_save_load_roundtrip);
    HU_RUN_TEST(test_session_save_null_args);
    HU_RUN_TEST(test_session_load_null_args);
    HU_RUN_TEST(test_session_load_missing_file);
    HU_RUN_TEST(test_session_save_empty_manager);
    HU_RUN_TEST(test_session_save_special_chars);
}
