#include "human/agent/checkpoint.h"
#include "test_framework.h"
#include <string.h>

static void *test_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void test_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}
static hu_allocator_t test_allocator = {.alloc = test_alloc, .free = test_free, .ctx = NULL};

static void test_checkpoint_store_init(void) {
    hu_checkpoint_store_t store;
    hu_checkpoint_store_init(&store, true, 5);
    HU_ASSERT_EQ(store.count, 0);
    HU_ASSERT_TRUE(store.auto_checkpoint);
    HU_ASSERT_EQ(store.interval_steps, 5);
}

static void test_checkpoint_save_and_load(void) {
    hu_checkpoint_store_t store;
    hu_checkpoint_store_init(&store, true, 0);

    static const char task[] = "task-001";
    static const char state[] = "{\"step\":3,\"context\":\"hello\"}";
    hu_error_t err = hu_checkpoint_save(&store, &test_allocator, task, 8, 3, HU_CHECKPOINT_ACTIVE,
                                        state, strlen(state));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(store.count, 1);

    hu_checkpoint_t loaded;
    err = hu_checkpoint_load(&store, task, 8, &loaded);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(loaded.task_id, "task-001");
    HU_ASSERT_EQ(loaded.step, 3);
    HU_ASSERT_EQ(loaded.status, HU_CHECKPOINT_ACTIVE);
    HU_ASSERT_NOT_NULL(loaded.state_json);
    HU_ASSERT_STR_CONTAINS(loaded.state_json, "hello");

    if (store.checkpoints[0].state_json)
        test_allocator.free(NULL, store.checkpoints[0].state_json,
                            store.checkpoints[0].state_json_len + 1);
}

static void test_checkpoint_update_existing(void) {
    hu_checkpoint_store_t store;
    hu_checkpoint_store_init(&store, true, 0);

    static const char task[] = "task-002";
    hu_checkpoint_save(&store, &test_allocator, task, 8, 1, HU_CHECKPOINT_ACTIVE, "{}", 2);
    HU_ASSERT_EQ(store.count, 1);

    static const char state2[] = "{\"step\":2}";
    hu_checkpoint_save(&store, &test_allocator, task, 8, 2, HU_CHECKPOINT_PAUSED, state2,
                       strlen(state2));
    HU_ASSERT_EQ(store.count, 1);

    hu_checkpoint_t loaded;
    hu_checkpoint_load(&store, task, 8, &loaded);
    HU_ASSERT_EQ(loaded.step, 2);
    HU_ASSERT_EQ(loaded.status, HU_CHECKPOINT_PAUSED);

    if (store.checkpoints[0].state_json)
        test_allocator.free(NULL, store.checkpoints[0].state_json,
                            store.checkpoints[0].state_json_len + 1);
}

static void test_checkpoint_load_latest(void) {
    hu_checkpoint_store_t store;
    hu_checkpoint_store_init(&store, true, 0);

    hu_checkpoint_save(&store, &test_allocator, "a", 1, 1, HU_CHECKPOINT_ACTIVE, NULL, 0);
    hu_checkpoint_save(&store, &test_allocator, "b", 1, 5, HU_CHECKPOINT_COMPLETED, NULL, 0);

    hu_checkpoint_t latest;
    hu_error_t err = hu_checkpoint_load_latest(&store, &latest);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(latest.updated_at > 0);
}

static void test_checkpoint_load_not_found(void) {
    hu_checkpoint_store_t store;
    hu_checkpoint_store_init(&store, true, 0);

    hu_checkpoint_t loaded;
    hu_error_t err = hu_checkpoint_load(&store, "nonexistent", 11, &loaded);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
}

static void test_checkpoint_latest_empty(void) {
    hu_checkpoint_store_t store;
    hu_checkpoint_store_init(&store, true, 0);

    hu_checkpoint_t latest;
    hu_error_t err = hu_checkpoint_load_latest(&store, &latest);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
}

static void test_checkpoint_should_save(void) {
    hu_checkpoint_store_t store;

    hu_checkpoint_store_init(&store, true, 0);
    HU_ASSERT_TRUE(hu_checkpoint_should_save(&store, 0));
    HU_ASSERT_TRUE(hu_checkpoint_should_save(&store, 1));
    HU_ASSERT_TRUE(hu_checkpoint_should_save(&store, 99));

    hu_checkpoint_store_init(&store, true, 3);
    HU_ASSERT_TRUE(hu_checkpoint_should_save(&store, 0));
    HU_ASSERT_FALSE(hu_checkpoint_should_save(&store, 1));
    HU_ASSERT_FALSE(hu_checkpoint_should_save(&store, 2));
    HU_ASSERT_TRUE(hu_checkpoint_should_save(&store, 3));
    HU_ASSERT_TRUE(hu_checkpoint_should_save(&store, 6));

    hu_checkpoint_store_init(&store, false, 0);
    HU_ASSERT_FALSE(hu_checkpoint_should_save(&store, 0));
}

static void test_checkpoint_status_names(void) {
    HU_ASSERT_STR_EQ(hu_checkpoint_status_name(HU_CHECKPOINT_ACTIVE), "active");
    HU_ASSERT_STR_EQ(hu_checkpoint_status_name(HU_CHECKPOINT_PAUSED), "paused");
    HU_ASSERT_STR_EQ(hu_checkpoint_status_name(HU_CHECKPOINT_COMPLETED), "completed");
    HU_ASSERT_STR_EQ(hu_checkpoint_status_name(HU_CHECKPOINT_FAILED), "failed");
}

static void test_checkpoint_null_args(void) {
    hu_checkpoint_store_init(NULL, true, 0);
    HU_ASSERT_EQ(hu_checkpoint_save(NULL, NULL, "x", 1, 0, HU_CHECKPOINT_ACTIVE, NULL, 0),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_checkpoint_load(NULL, "x", 1, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_checkpoint_load_latest(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_FALSE(hu_checkpoint_should_save(NULL, 0));
}

/* Two saves to one task plus one to another, then deinit: zero live bytes,
 * an empty store, and the configuration kept. */
static void test_checkpoint_store_deinit_frees_every_state(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    HU_ASSERT_NOT_NULL(ta);
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
    hu_checkpoint_store_t store;
    hu_checkpoint_store_init(&store, true, 5);
    static const char s1[] = "{\"a\":1}";
    static const char s2[] = "{\"a\":2}";
    HU_ASSERT_EQ(
        hu_checkpoint_save(&store, &alloc, "t1", 2, 1, HU_CHECKPOINT_ACTIVE, s1, sizeof(s1) - 1),
        HU_OK);
    HU_ASSERT_EQ(
        hu_checkpoint_save(&store, &alloc, "t1", 2, 2, HU_CHECKPOINT_ACTIVE, s2, sizeof(s2) - 1),
        HU_OK);
    HU_ASSERT_EQ(hu_checkpoint_save(&store, &alloc, "t2", 2, 1, HU_CHECKPOINT_ACTIVE, "{}", 2),
                 HU_OK);
    HU_ASSERT_EQ(store.count, 2);
    HU_ASSERT_GT(hu_tracking_allocator_leaks(ta), 0); /* precondition: live state copies */
    hu_checkpoint_store_deinit(&store, &alloc);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    HU_ASSERT_EQ(store.count, 0);
    HU_ASSERT_TRUE(store.auto_checkpoint);
    HU_ASSERT_EQ(store.interval_steps, 5);
    hu_checkpoint_store_deinit(&store, &alloc); /* idempotent */
    hu_checkpoint_store_deinit(NULL, &alloc);
    hu_checkpoint_store_deinit(&store, NULL);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

void run_checkpoint_tests(void) {
    HU_TEST_SUITE("Checkpoint");
    HU_RUN_TEST(test_checkpoint_store_init);
    HU_RUN_TEST(test_checkpoint_save_and_load);
    HU_RUN_TEST(test_checkpoint_update_existing);
    HU_RUN_TEST(test_checkpoint_load_latest);
    HU_RUN_TEST(test_checkpoint_load_not_found);
    HU_RUN_TEST(test_checkpoint_latest_empty);
    HU_RUN_TEST(test_checkpoint_should_save);
    HU_RUN_TEST(test_checkpoint_status_names);
    HU_RUN_TEST(test_checkpoint_null_args);
    HU_RUN_TEST(test_checkpoint_store_deinit_frees_every_state);
}
