#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory/cross_channel.h"
#include "human/persona.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static sqlite3 *test_db_create(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK)
        return NULL;
    return db;
}

static void test_db_close(sqlite3 *db) {
    if (db)
        sqlite3_close(db);
}

static void test_db_create_reflection_table(sqlite3 *db) {
    const char *sql = "CREATE TABLE reflection_patterns ("
                      "  id TEXT PRIMARY KEY, "
                      "  observation TEXT NOT NULL, "
                      "  observed_at_ms INTEGER NOT NULL, "
                      "  confidence REAL NOT NULL, "
                      "  channels_json TEXT "
                      ")";
    char *errmsg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
    }
}

static void test_collect_returns_reflection_patterns(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = test_db_create();
    HU_ASSERT_NOT_NULL(db);

    test_db_create_reflection_table(db);

    int64_t now_ms = 1000000000LL;
    int64_t obs_ms = now_ms - (2 * 3600 * 1000);
    const char *insert_sql =
        "INSERT INTO reflection_patterns "
        "(id, observation, observed_at_ms, confidence) "
        "VALUES ('pat_001', 'User tends to be more stressed on Fridays', ?, 0.85)";
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, obs_ms);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);

    hu_cross_channel_item_t *items = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_cross_channel_collect(&alloc, db, "imessage", "contact_self", now_ms, 10,
                                          &items, &count),
                 HU_OK);

    HU_ASSERT_GT(count, 0);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_EQ(items[0].source_type, HU_XCHAN_REFLECTION_PATTERN);
    HU_ASSERT_STR_EQ(items[0].item_id, "pat_001");
    HU_ASSERT_STR_CONTAINS(items[0].text, "stressed");

    hu_cross_channel_items_free(&alloc, items, count);
    test_db_close(db);
}

static void test_collect_skips_reflection_table_when_absent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = test_db_create();
    HU_ASSERT_NOT_NULL(db);

    hu_cross_channel_item_t *items = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_cross_channel_collect(&alloc, db, "imessage", "contact_self", 1000000000LL, 10,
                                          &items, &count),
                 HU_OK);

    HU_ASSERT_EQ(count, 0);

    hu_cross_channel_items_free(&alloc, items, count);
    test_db_close(db);
}

static void test_format_includes_origin_channel_and_relative_time(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_cross_channel_item_t items[2] = {{0}};
    int64_t now_ms = 1000000000LL;

    items[0].source_type = HU_XCHAN_FACT;
    strcpy(items[0].item_id, "fact_001");
    items[0].text = (char *)"User mentioned enjoying coffee";
    items[0].text_len = strlen(items[0].text);
    strcpy(items[0].origin_channel, "telegram");
    items[0].observed_at_ms = now_ms - (3600 * 1000);
    items[0].confidence = 0.9;

    items[1].source_type = HU_XCHAN_FACT;
    strcpy(items[1].item_id, "fact_002");
    items[1].text = (char *)"User is planning a trip next month";
    items[1].text_len = strlen(items[1].text);
    strcpy(items[1].origin_channel, "imessage");
    items[1].observed_at_ms = now_ms - (24 * 3600 * 1000);
    items[1].confidence = 0.8;

    char *formatted = NULL;
    size_t formatted_len = 0;
    HU_ASSERT_EQ(hu_cross_channel_format(&alloc, now_ms, items, 2, &formatted, &formatted_len),
                 HU_OK);

    HU_ASSERT_NOT_NULL(formatted);
    HU_ASSERT_GT(formatted_len, 0);

    HU_ASSERT_STR_CONTAINS(formatted, "From telegram");
    HU_ASSERT_STR_CONTAINS(formatted, "From imessage");
    HU_ASSERT_STR_CONTAINS(formatted, "h ago");
    HU_ASSERT_STR_CONTAINS(formatted, "d ago");

    if (formatted)
        alloc.free(alloc.ctx, formatted, formatted_len);
}

static void test_format_when_produces_relative_times(void) {
    char buf[64];
    int64_t now_ms = 1000000000LL;

    hu_cross_channel_format_when(buf, sizeof(buf), now_ms - 30000, now_ms);
    HU_ASSERT_STR_EQ(buf, "just now");

    hu_cross_channel_format_when(buf, sizeof(buf), now_ms - (5 * 60 * 1000), now_ms);
    HU_ASSERT_STR_CONTAINS(buf, "m ago");

    hu_cross_channel_format_when(buf, sizeof(buf), now_ms - (3 * 3600 * 1000), now_ms);
    HU_ASSERT_STR_CONTAINS(buf, "h ago");

    hu_cross_channel_format_when(buf, sizeof(buf), now_ms - (5 * 24 * 3600 * 1000), now_ms);
    HU_ASSERT_STR_CONTAINS(buf, "d ago");
}

void run_cross_channel_pipeline_tests(void) {
    HU_TEST_SUITE("cross_channel_pipeline");
    HU_RUN_TEST(test_collect_returns_reflection_patterns);
    HU_RUN_TEST(test_collect_skips_reflection_table_when_absent);
    HU_RUN_TEST(test_format_includes_origin_channel_and_relative_time);
    HU_RUN_TEST(test_format_when_produces_relative_times);
}

#else
void run_cross_channel_pipeline_tests(void) {
    (void)0;
}
#endif
