#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/prospective.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

static void prospective_store_and_check_triggers_finds_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t id = 0;
    hu_error_t err = hu_prospective_store(db, "topic", 5, "meeting", 7, "remind about report", 19,
                                          "contact_a", 9, 0, &id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(id > 0);

    hu_prospective_entry_t *out = NULL;
    size_t count = 0;
    int64_t now_ts = (int64_t)time(NULL) + 86400;
    err = hu_prospective_check_triggers(&alloc, db, "topic", "user mentioned meeting tomorrow", 28,
                                        "contact_a", 9, now_ts, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(out[0].action, "remind about report");
    HU_ASSERT_STR_EQ(out[0].trigger_value, "meeting");

    alloc.free(alloc.ctx, out, count * sizeof(hu_prospective_entry_t));
    mem.vtable->deinit(mem.ctx);
}

static void prospective_mark_fired_excludes_from_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t id = 0;
    HU_ASSERT_EQ(hu_prospective_store(db, "event", 5, "birthday", 8, "send card", 9, NULL, 0, 0,
                                      &id),
                 HU_OK);

    hu_prospective_entry_t *out = NULL;
    size_t count = 0;
    int64_t now_ts = (int64_t)time(NULL) + 86400;
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "event", "birthday party", 14, NULL, 0,
                                               now_ts, &out, &count),
                 HU_OK);
    HU_ASSERT_EQ(count, 1u);
    int64_t found_id = out[0].id;
    alloc.free(alloc.ctx, out, count * sizeof(hu_prospective_entry_t));

    HU_ASSERT_EQ(hu_prospective_mark_fired(db, found_id), HU_OK);

    out = NULL;
    count = 0;
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "event", "birthday party", 14, NULL, 0,
                                               now_ts, &out, &count),
                 HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(out);

    mem.vtable->deinit(mem.ctx);
}

static void prospective_expired_entry_excluded(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t id = 0;
    int64_t past_expiry = (int64_t)time(NULL) - 3600;
    HU_ASSERT_EQ(hu_prospective_store(db, "topic", 5, "deadline", 8, "follow up", 9, "user_b", 5,
                                      past_expiry, &id),
                 HU_OK);

    hu_prospective_entry_t *out = NULL;
    size_t count = 0;
    int64_t now_ts = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "topic", "deadline passed", 14, "user_b",
                                               5, now_ts, &out, &count),
                 HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(out);

    mem.vtable->deinit(mem.ctx);
}

void run_prospective_tests(void) {
    HU_TEST_SUITE("prospective");
    HU_RUN_TEST(prospective_store_and_check_triggers_finds_match);
    HU_RUN_TEST(prospective_mark_fired_excludes_from_check);
    HU_RUN_TEST(prospective_expired_entry_excluded);
}

#else

void run_prospective_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
