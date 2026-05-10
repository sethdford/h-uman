#include "human/context/contact_style_overlay.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static void contact_style_temporal_mood_contains_window(void) {
    char buf[128];
    HU_ASSERT_EQ(0u, hu_temporal_mood_build(12, NULL, sizeof(buf)));
    HU_ASSERT_EQ(0u, hu_temporal_mood_build(12, buf, 32));

    HU_ASSERT_GT(hu_temporal_mood_build(3, buf, sizeof(buf)), 0u);
    HU_ASSERT_NOT_NULL(strstr(buf, "late"));
    HU_ASSERT_GT(hu_temporal_mood_build(12, buf, sizeof(buf)), 0u);
    HU_ASSERT_NOT_NULL(strstr(buf, "work hours"));
    HU_ASSERT_GT(hu_temporal_mood_build(19, buf, sizeof(buf)), 0u);
    HU_ASSERT_NOT_NULL(strstr(buf, "evening"));
}

#ifdef HU_ENABLE_SQLITE

#include "human/memory.h"
#include "human/memory/contact_graph.h"
#include "human/memory/emotional_moments.h"
#include <sqlite3.h>

static void contact_style_overlay_from_memory_tables(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_contact_graph_init(&alloc, db), HU_OK);
    HU_ASSERT_EQ(hu_contact_graph_link(db, "user_a", "imessage", "+15551234", "Alex Person", 1.0),
                  HU_OK);

    char *err = NULL;
    int rc = sqlite3_exec(
        db,
        "INSERT INTO contact_relationships(contact_id,person_name,role,last_mentioned) "
        "VALUES('user_a','self','close friend',1700000000);"
        "INSERT OR REPLACE INTO style_fingerprints(contact_id,uses_lowercase,uses_periods,"
        "laugh_style,avg_message_length) VALUES('user_a',1,0,'lol',52);",
        NULL, NULL, &err);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_NULL(err);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_contact_style_overlay_build(&alloc, &mem, "user_a", 6, &out, &out_len),
                  HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 20u);
    HU_ASSERT_NOT_NULL(strstr(out, "Alex Person"));
    HU_ASSERT_NOT_NULL(strstr(out, "close friend"));
    HU_ASSERT_NOT_NULL(strstr(out, "lol"));

    alloc.free(alloc.ctx, out, out_len + 1);
    mem.vtable->deinit(mem.ctx);
}

static void contact_style_emotional_context_lists_rows(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_emotional_moment_record(&alloc, &mem, "user_b", 6, "deadline", 8, "stressed",
                                             8, 0.75f),
                  HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_contact_emotional_context_build(&alloc, &mem, "user_b", 6, 5, &out, &out_len),
                  HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "deadline"));
    HU_ASSERT_NOT_NULL(strstr(out, "stressed"));
    HU_ASSERT_NOT_NULL(strstr(out, "0.8"));

    alloc.free(alloc.ctx, out, out_len + 1);
    mem.vtable->deinit(mem.ctx);
}

#endif /* HU_ENABLE_SQLITE */

void run_contact_style_overlay_tests(void) {
    HU_TEST_SUITE("contact_style_overlay");
    HU_RUN_TEST(contact_style_temporal_mood_contains_window);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(contact_style_overlay_from_memory_tables);
    HU_RUN_TEST(contact_style_emotional_context_lists_rows);
#endif
}
