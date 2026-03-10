#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/emotional_moments.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

static void emotional_moment_record_and_get_due(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_emotional_moment_record(&alloc, &mem, "contact_a", 9, "work stress", 11,
                                                  "stressed", 8, 0.8f);
    HU_ASSERT_EQ(err, HU_OK);

    /* In HU_IS_TEST, follow_up_date = created_at + 86400 (1 day). Advance time past that. */
    int64_t now_ts = (int64_t)time(NULL) + 86401;
    hu_emotional_moment_t *due = NULL;
    size_t due_count = 0;
    err = hu_emotional_moment_get_due(&alloc, &mem, now_ts, &due, &due_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(due);
    HU_ASSERT_EQ(due_count, 1u);
    HU_ASSERT_STR_EQ(due[0].contact_id, "contact_a");
    HU_ASSERT_STR_EQ(due[0].topic, "work stress");
    HU_ASSERT_STR_EQ(due[0].emotion, "stressed");
    HU_ASSERT_FALSE(due[0].followed_up);

    alloc.free(alloc.ctx, due, due_count * sizeof(hu_emotional_moment_t));
    mem.vtable->deinit(mem.ctx);
}

static void emotional_moment_mark_followed_up_excludes_from_get_due(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    (void)hu_emotional_moment_record(&alloc, &mem, "contact_b", 9, "family issue", 12, "sad", 3,
                                     0.7f);

    int64_t now_ts = (int64_t)time(NULL) + 86401;
    hu_emotional_moment_t *due = NULL;
    size_t due_count = 0;
    HU_ASSERT_EQ(hu_emotional_moment_get_due(&alloc, &mem, now_ts, &due, &due_count), HU_OK);
    HU_ASSERT_EQ(due_count, 1u);
    int64_t id = due[0].id;
    alloc.free(alloc.ctx, due, due_count * sizeof(hu_emotional_moment_t));

    HU_ASSERT_EQ(hu_emotional_moment_mark_followed_up(&mem, id), HU_OK);

    due = NULL;
    due_count = 0;
    HU_ASSERT_EQ(hu_emotional_moment_get_due(&alloc, &mem, now_ts, &due, &due_count), HU_OK);
    HU_ASSERT_EQ(due_count, 0u);
    HU_ASSERT_NULL(due);

    mem.vtable->deinit(mem.ctx);
}

static void emotional_moment_same_contact_topic_within_7_days_skips_duplicate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_emotional_moment_record(&alloc, &mem, "contact_c", 9, "job interview", 11,
                                            "anxious", 7, 0.6f),
                 HU_OK);

    /* Second record with same contact+topic within 7 days should skip (no error, no duplicate) */
    hu_error_t err = hu_emotional_moment_record(&alloc, &mem, "contact_c", 9, "job interview", 11,
                                                 "anxious", 7, 0.5f);
    HU_ASSERT_EQ(err, HU_OK);

    /* Should still have only one due (follow_up is 1 day out) */
    int64_t now_ts = (int64_t)time(NULL) + 86401;
    hu_emotional_moment_t *due = NULL;
    size_t due_count = 0;
    HU_ASSERT_EQ(hu_emotional_moment_get_due(&alloc, &mem, now_ts, &due, &due_count), HU_OK);
    HU_ASSERT_EQ(due_count, 1u);
    alloc.free(alloc.ctx, due, due_count * sizeof(hu_emotional_moment_t));

    mem.vtable->deinit(mem.ctx);
}

static void emotional_moment_different_topic_records(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_emotional_moment_record(&alloc, &mem, "contact_d", 9, "topic_a", 7, "sad", 3,
                                            0.5f),
                 HU_OK);
    HU_ASSERT_EQ(hu_emotional_moment_record(&alloc, &mem, "contact_d", 9, "topic_b", 7, "worried",
                                            7, 0.6f),
                 HU_OK);

    int64_t now_ts = (int64_t)time(NULL) + 86401;
    hu_emotional_moment_t *due = NULL;
    size_t due_count = 0;
    HU_ASSERT_EQ(hu_emotional_moment_get_due(&alloc, &mem, now_ts, &due, &due_count), HU_OK);
    HU_ASSERT_EQ(due_count, 2u);
    alloc.free(alloc.ctx, due, due_count * sizeof(hu_emotional_moment_t));

    mem.vtable->deinit(mem.ctx);
}

static void emotional_moment_none_memory_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);

    hu_error_t err = hu_emotional_moment_record(&alloc, &mem, "x", 1, "y", 1, "z", 1, 0.5f);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);

    hu_emotional_moment_t *due = NULL;
    size_t due_count = 0;
    err = hu_emotional_moment_get_due(&alloc, &mem, (int64_t)time(NULL), &due, &due_count);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);

    err = hu_emotional_moment_mark_followed_up(&mem, 1);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);

    mem.vtable->deinit(mem.ctx);
}

void run_emotional_moments_tests(void) {
    HU_TEST_SUITE("emotional_moments");
    HU_RUN_TEST(emotional_moment_record_and_get_due);
    HU_RUN_TEST(emotional_moment_mark_followed_up_excludes_from_get_due);
    HU_RUN_TEST(emotional_moment_same_contact_topic_within_7_days_skips_duplicate);
    HU_RUN_TEST(emotional_moment_different_topic_records);
    HU_RUN_TEST(emotional_moment_none_memory_returns_not_supported);
}

#else

void run_emotional_moments_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
