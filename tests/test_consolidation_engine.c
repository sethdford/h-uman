#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/consolidation_engine.h"
#include "human/memory/episodic.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

/* Nightly: insert 2 similar episodes (substring match), run nightly → one deleted. */
static void consolidation_engine_nightly_deduplicates_similar_episodes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t id1 = 0, id2 = 0;
    HU_ASSERT_EQ(hu_episode_store_insert(&alloc, db, "contact_a", 9, "Meeting about project", 21,
                                         NULL, 0, "discussed scope", 15, 0.6, "conversation", 12,
                                         &id1),
                 HU_OK);
    HU_ASSERT_EQ(hu_episode_store_insert(&alloc, db, "contact_a", 9, "Meeting", 7,
                                         NULL, 0, "quick sync", 10, 0.8, "conversation", 12,
                                         &id2),
                 HU_OK);
    HU_ASSERT_TRUE(id1 > 0);
    HU_ASSERT_TRUE(id2 > 0);

    hu_episode_sqlite_t *out = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_episode_get_by_contact(&alloc, db, "contact_a", 9, 10, 0, &out, &count),
                 HU_OK);
    HU_ASSERT_EQ(count, 2u);
    hu_episode_free(&alloc, out, count);

    hu_consolidation_engine_t engine = {.alloc = &alloc, .db = db};
    int64_t now_ts = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_consolidation_engine_nightly(&engine, now_ts), HU_OK);

    out = NULL;
    count = 0;
    HU_ASSERT_EQ(hu_episode_get_by_contact(&alloc, db, "contact_a", 9, 10, 0, &out, &count),
                 HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_TRUE(out[0].impact_score >= 0.7);
    hu_episode_free(&alloc, out, count);
    mem.vtable->deinit(mem.ctx);
}

/* Weekly: insert 6 episodes for same contact → summary created. */
static void consolidation_engine_weekly_creates_summary_when_more_than_five(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    for (int i = 0; i < 6; i++) {
        char summary[64];
        char key[64];
        int n = snprintf(summary, sizeof(summary), "Chat %d with contact", i);
        int k = snprintf(key, sizeof(key), "moment_%d", i);
        int64_t id = 0;
        HU_ASSERT_EQ(hu_episode_store_insert(&alloc, db, "contact_b", 9, summary, (size_t)n,
                                             NULL, 0, key, (size_t)k, 0.5, "conversation", 12,
                                             &id),
                     HU_OK);
        HU_ASSERT_TRUE(id > 0);
    }

    hu_consolidation_engine_t engine = {.alloc = &alloc, .db = db};
    int64_t now_ts = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_consolidation_engine_weekly(&engine, now_ts), HU_OK);

    hu_episode_sqlite_t *out = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_episode_get_by_contact(&alloc, db, "contact_b", 9, 20, 0, &out, &count),
                 HU_OK);
    HU_ASSERT_TRUE(count >= 7u);
    bool found_summary = false;
    for (size_t i = 0; i < count; i++) {
        if (strstr(out[i].summary, "This week with") != NULL &&
            strstr(out[i].source, "weekly_summary") != NULL) {
            found_summary = true;
            break;
        }
    }
    HU_ASSERT_TRUE(found_summary);
    hu_episode_free(&alloc, out, count);
    mem.vtable->deinit(mem.ctx);
}

/* Monthly: insert old low-salience episode → deleted after monthly. */
static void consolidation_engine_monthly_deletes_old_low_salience(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t now_ts = (int64_t)time(NULL);
    int64_t old_ts = now_ts - (91 * 86400);

    sqlite3_stmt *ins = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO episodes(contact_id,summary,impact_score,"
                                "salience_score,source,created_at) VALUES(?,?,?,?,?,?)",
                                -1, &ins, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(ins, 1, "contact_c", 9, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "Old low salience", 16, SQLITE_STATIC);
    sqlite3_bind_double(ins, 3, 0.3);
    sqlite3_bind_double(ins, 4, 0.03);
    sqlite3_bind_text(ins, 5, "conversation", 12, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 6, old_ts);
    rc = sqlite3_step(ins);
    HU_ASSERT_EQ(rc, SQLITE_DONE);
    sqlite3_finalize(ins);

    hu_consolidation_engine_t engine = {.alloc = &alloc, .db = db};
    HU_ASSERT_EQ(hu_consolidation_engine_monthly(&engine, now_ts), HU_OK);

    hu_episode_sqlite_t *out = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_episode_get_by_contact(&alloc, db, "contact_c", 9, 10, 0, &out, &count),
                 HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(out);
    mem.vtable->deinit(mem.ctx);
}

/* run_scheduled: verify time gating — recent last_* does not run tasks. */
static void consolidation_engine_run_scheduled_respects_time_gating(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t id1 = 0, id2 = 0;
    HU_ASSERT_EQ(hu_episode_store_insert(&alloc, db, "contact_d", 9, "Sync", 4,
                                         NULL, 0, "discussed", 9, 0.5, "conversation", 12, &id1),
                 HU_OK);
    HU_ASSERT_EQ(hu_episode_store_insert(&alloc, db, "contact_d", 9, "Sync call", 8,
                                         NULL, 0, "discussed", 9, 0.6, "conversation", 12, &id2),
                 HU_OK);

    int64_t now_ts = (int64_t)time(NULL);
    int64_t one_hour_ago = now_ts - 3600;

    hu_consolidation_engine_t engine = {.alloc = &alloc, .db = db};
    HU_ASSERT_EQ(hu_consolidation_engine_run_scheduled(&engine, now_ts, one_hour_ago, one_hour_ago,
                                                      one_hour_ago),
                 HU_OK);

    hu_episode_sqlite_t *out = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_episode_get_by_contact(&alloc, db, "contact_d", 9, 10, 0, &out, &count),
                 HU_OK);
    HU_ASSERT_EQ(count, 2u);
    hu_episode_free(&alloc, out, count);

    HU_ASSERT_EQ(hu_consolidation_engine_run_scheduled(&engine, now_ts, 0, 0, 0), HU_OK);

    out = NULL;
    count = 0;
    HU_ASSERT_EQ(hu_episode_get_by_contact(&alloc, db, "contact_d", 9, 10, 0, &out, &count),
                 HU_OK);
    HU_ASSERT_EQ(count, 1u);
    hu_episode_free(&alloc, out, count);
    mem.vtable->deinit(mem.ctx);
}

void run_consolidation_engine_tests(void) {
    HU_TEST_SUITE("consolidation_engine");
    HU_RUN_TEST(consolidation_engine_nightly_deduplicates_similar_episodes);
    HU_RUN_TEST(consolidation_engine_weekly_creates_summary_when_more_than_five);
    HU_RUN_TEST(consolidation_engine_monthly_deletes_old_low_salience);
    HU_RUN_TEST(consolidation_engine_run_scheduled_respects_time_gating);
}

#else

typedef int hu_consolidation_engine_test_unused_;

void run_consolidation_engine_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
