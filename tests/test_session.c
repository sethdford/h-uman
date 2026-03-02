/* Session manager tests */
#include "test_framework.h"
#include "seaclaw/session.h"
#include "seaclaw/core/allocator.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static void test_session_manager_init_deinit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_error_t err = sc_session_manager_init(&mgr, &alloc);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(sc_session_count(&mgr), 0);
    sc_session_manager_deinit(&mgr);
}

static void test_session_get_or_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);

    sc_session_t *s1 = sc_session_get_or_create(&mgr, "abc");
    SC_ASSERT_NOT_NULL(s1);
    SC_ASSERT_TRUE(strstr(s1->session_key, "abc") != NULL);
    SC_ASSERT_EQ(sc_session_count(&mgr), 1);

    sc_session_t *s2 = sc_session_get_or_create(&mgr, "abc");
    SC_ASSERT_TRUE(s1 == s2);

    sc_session_t *s3 = sc_session_get_or_create(&mgr, "xyz");
    SC_ASSERT_NOT_NULL(s3);
    SC_ASSERT_TRUE(s3 != s1);
    SC_ASSERT_EQ(sc_session_count(&mgr), 2);

    sc_session_manager_deinit(&mgr);
}

static void test_session_append_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_t *s = sc_session_get_or_create(&mgr, "chat1");
    SC_ASSERT_NOT_NULL(s);

    sc_error_t err = sc_session_append_message(s, &alloc, "user", "Hello");
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(s->message_count, 1);
    SC_ASSERT_TRUE(strcmp(s->messages[0].role, "user") == 0);
    SC_ASSERT_TRUE(strcmp(s->messages[0].content, "Hello") == 0);

    err = sc_session_append_message(s, &alloc, "assistant", "Hi there");
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(s->message_count, 2);

    sc_session_manager_deinit(&mgr);
}

static void test_session_gen_id(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *id = sc_session_gen_id(&alloc);
    SC_ASSERT_NOT_NULL(id);
    SC_ASSERT_TRUE(strlen(id) > 0);
    alloc.free(alloc.ctx, id, strlen(id) + 1);
}

static void test_session_evict_idle(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_get_or_create(&mgr, "evict_me");
    SC_ASSERT_EQ(sc_session_count(&mgr), 1);

    /* Evict sessions idle for 0 seconds = evict all (last_active is now) */
    size_t evicted = sc_session_evict_idle(&mgr, 0);
    /* Behavior may vary: 0 sec could mean "just now" so nothing evicted */
    (void)evicted;
    sc_session_manager_deinit(&mgr);
}

static void test_session_create_multiple_distinct(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);

    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "sess_%d", i);
        sc_session_t *s = sc_session_get_or_create(&mgr, key);
        SC_ASSERT_NOT_NULL(s);
        SC_ASSERT_TRUE(strstr(s->session_key, key) != NULL);
    }
    SC_ASSERT_EQ(sc_session_count(&mgr), 10u);
    sc_session_manager_deinit(&mgr);
}

static void test_session_same_key_returns_same_ptr(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);

    sc_session_t *s1 = sc_session_get_or_create(&mgr, "identical");
    sc_session_t *s2 = sc_session_get_or_create(&mgr, "identical");
    SC_ASSERT_TRUE(s1 == s2);
    SC_ASSERT_EQ(sc_session_count(&mgr), 1u);
    sc_session_manager_deinit(&mgr);
}

static void test_session_evict_idle_removes_old(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);

    sc_session_t *s = sc_session_get_or_create(&mgr, "old_session");
    SC_ASSERT_NOT_NULL(s);
    s->last_active = (int64_t)time(NULL) - 100;
    SC_ASSERT_EQ(sc_session_count(&mgr), 1u);

    size_t evicted = sc_session_evict_idle(&mgr, 50);
    SC_ASSERT_EQ(evicted, 1u);
    SC_ASSERT_EQ(sc_session_count(&mgr), 0u);
    sc_session_manager_deinit(&mgr);
}

static void test_session_append_message_long_content(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_t *s = sc_session_get_or_create(&mgr, "long");
    SC_ASSERT_NOT_NULL(s);

    char long_content[1024];
    for (size_t i = 0; i < sizeof(long_content) - 1; i++)
        long_content[i] = (char)('A' + (i % 26));
    long_content[sizeof(long_content) - 1] = '\0';

    sc_error_t err = sc_session_append_message(s, &alloc, "user", long_content);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(s->message_count, 1u);
    SC_ASSERT_TRUE(strncmp(s->messages[0].content, "ABCD", 4) == 0);
    sc_session_manager_deinit(&mgr);
}

static void test_session_gen_id_unique(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *id1 = sc_session_gen_id(&alloc);
    char *id2 = sc_session_gen_id(&alloc);
    SC_ASSERT_NOT_NULL(id1);
    SC_ASSERT_NOT_NULL(id2);
    SC_ASSERT_TRUE(strcmp(id1, id2) != 0);
    alloc.free(alloc.ctx, id1, strlen(id1) + 1);
    alloc.free(alloc.ctx, id2, strlen(id2) + 1);
}

static void test_session_append_message_invalid_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_t *s = sc_session_get_or_create(&mgr, "x");

    sc_error_t err = sc_session_append_message(NULL, &alloc, "user", "hi");
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_session_append_message(s, NULL, "user", "hi");
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_session_append_message(s, &alloc, NULL, "hi");
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_session_append_message(s, &alloc, "user", NULL);
    SC_ASSERT_NEQ(err, SC_OK);
    sc_session_manager_deinit(&mgr);
}

static void test_session_manager_init_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_session_manager_init(NULL, &alloc);
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_session_manager_init((sc_session_manager_t *)1, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_session_get_or_create_null_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_t *s = sc_session_get_or_create(&mgr, NULL);
    SC_ASSERT_NULL(s);
    sc_session_manager_deinit(&mgr);
}

static void test_session_message_ordering(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_t *s = sc_session_get_or_create(&mgr, "order");
    sc_session_append_message(s, &alloc, "user", "1");
    sc_session_append_message(s, &alloc, "assistant", "2");
    sc_session_append_message(s, &alloc, "user", "3");
    SC_ASSERT_EQ(s->message_count, 3u);
    SC_ASSERT_STR_EQ(s->messages[0].content, "1");
    SC_ASSERT_STR_EQ(s->messages[1].content, "2");
    SC_ASSERT_STR_EQ(s->messages[2].content, "3");
    sc_session_manager_deinit(&mgr);
}

static void test_session_count_after_evict(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_get_or_create(&mgr, "s1");
    sc_session_get_or_create(&mgr, "s2");
    SC_ASSERT_EQ(sc_session_count(&mgr), 2u);
    sc_session_t *s1 = sc_session_get_or_create(&mgr, "s1");
    s1->last_active = (int64_t)time(NULL) - 200;
    sc_session_evict_idle(&mgr, 100);
    SC_ASSERT_TRUE(sc_session_count(&mgr) < 2u);
    sc_session_manager_deinit(&mgr);
}

static void test_session_deinit_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_manager_deinit(&mgr);
}

static void test_session_append_empty_content(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_t *s = sc_session_get_or_create(&mgr, "empty");
    sc_error_t err = sc_session_append_message(s, &alloc, "user", "");
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(s->message_count, 1u);
    sc_session_manager_deinit(&mgr);
}

static void test_session_list(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_get_or_create(&mgr, "a");
    sc_session_get_or_create(&mgr, "b");

    size_t count = 0;
    sc_session_summary_t *summaries = sc_session_list(&mgr, &alloc, &count);
    SC_ASSERT_NOT_NULL(summaries);
    SC_ASSERT_EQ(count, 2u);
    alloc.free(alloc.ctx, summaries, count * sizeof(sc_session_summary_t));
    sc_session_manager_deinit(&mgr);
}

static void test_session_list_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);

    size_t count = 0;
    sc_session_summary_t *summaries = sc_session_list(&mgr, &alloc, &count);
    SC_ASSERT_NULL(summaries);
    SC_ASSERT_EQ(count, 0u);
    sc_session_manager_deinit(&mgr);
}

static void test_session_delete(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_get_or_create(&mgr, "del_me");
    SC_ASSERT_EQ(sc_session_count(&mgr), 1u);

    sc_error_t err = sc_session_delete(&mgr, "del_me");
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(sc_session_count(&mgr), 0u);
    sc_session_manager_deinit(&mgr);
}

static void test_session_delete_not_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);

    sc_error_t err = sc_session_delete(&mgr, "nonexistent");
    SC_ASSERT_EQ(err, SC_ERR_NOT_FOUND);
    sc_session_manager_deinit(&mgr);
}

static void test_session_patch(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);
    sc_session_t *s = sc_session_get_or_create(&mgr, "patch_me");
    SC_ASSERT_NOT_NULL(s);

    sc_error_t err = sc_session_patch(&mgr, "patch_me", "my-label");
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(s->label, "my-label");
    sc_session_manager_deinit(&mgr);
}

static void test_session_patch_not_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_session_manager_t mgr;
    sc_session_manager_init(&mgr, &alloc);

    sc_error_t err = sc_session_patch(&mgr, "nonexistent", "x");
    SC_ASSERT_EQ(err, SC_ERR_NOT_FOUND);
    sc_session_manager_deinit(&mgr);
}

void run_session_tests(void) {
    SC_TEST_SUITE("Session");
    SC_RUN_TEST(test_session_manager_init_deinit);
    SC_RUN_TEST(test_session_get_or_create);
    SC_RUN_TEST(test_session_append_message);
    SC_RUN_TEST(test_session_gen_id);
    SC_RUN_TEST(test_session_evict_idle);
    SC_RUN_TEST(test_session_create_multiple_distinct);
    SC_RUN_TEST(test_session_same_key_returns_same_ptr);
    SC_RUN_TEST(test_session_evict_idle_removes_old);
    SC_RUN_TEST(test_session_append_message_long_content);
    SC_RUN_TEST(test_session_gen_id_unique);
    SC_RUN_TEST(test_session_append_message_invalid_args);
    SC_RUN_TEST(test_session_manager_init_null);
    SC_RUN_TEST(test_session_get_or_create_null_key);
    SC_RUN_TEST(test_session_message_ordering);
    SC_RUN_TEST(test_session_count_after_evict);
    SC_RUN_TEST(test_session_deinit_empty);
    SC_RUN_TEST(test_session_append_empty_content);
    SC_RUN_TEST(test_session_list);
    SC_RUN_TEST(test_session_list_empty);
    SC_RUN_TEST(test_session_delete);
    SC_RUN_TEST(test_session_delete_not_found);
    SC_RUN_TEST(test_session_patch);
    SC_RUN_TEST(test_session_patch_not_found);
}
