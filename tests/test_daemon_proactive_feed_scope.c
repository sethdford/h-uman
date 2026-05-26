/* tests/test_daemon_proactive_feed_scope.c
 *
 * Sprint 59 Phase C (2026-05-26 Annie/Mindy/Betty incident) — pin
 * per-contact scope for the proactive feed-aware bring-up context.
 *
 * Reference: docs/plans/2026-05-26-sprint-59-outbound-safety/design.md
 * Part 1. Root cause: hu_daemon_proactive_prompt_for_contact called
 * hu_feed_processor_get_all_recent, which returned items from EVERY
 * contact. Combined with the emotional-state recorder keying topics by
 * the proactive recipient's contact_id, this caused one "lonely" topic
 * in Mindy's feed to seed three rows in emotional_moments — for Mindy,
 * Betty, AND Annie.
 *
 * These tests pin the contract of the extracted helper
 * hu_daemon_proactive_get_contact_feed_items: it must return items
 * scoped to cp->contact_id only, never bleed in other contacts'
 * feed items, even when those items are more recent.
 *
 * Gated on HU_ENABLE_SQLITE in CMakeLists.txt; tests/test_main.c mirrors
 * the gate on the forward decl + call site.
 */

#include "human/core/allocator.h"
#include "human/daemon_proactive.h"
#include "human/feeds/processor.h"
#include "human/memory.h"
#include "human/persona.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>

/* Helper — insert one row into the feed_items table the production code
 * reads from. Matches the schema in src/feeds/processor.c. */
static void insert_feed(sqlite3 *db, const char *source, const char *contact_id,
                        const char *content, int64_t ingested_at) {
    const char *sql =
        "INSERT INTO feed_items (source, contact_id, content_type, content, url, ingested_at) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, "post", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, content, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, ingested_at);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

/* Positive contract: helper returns the target contact's row only, even
 * when other contacts' rows are more recent. This is the regression test
 * for the Annie/Mindy/Betty bleed. */
static void test_get_contact_feed_items_returns_only_target_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    /* Three contacts, distinct unique markers, Annie's row is the OLDEST
     * — so a recency-only query (get_all_recent) would return Mindy and
     * Betty before Annie. The per-contact query must still pick Annie. */
    insert_feed(db, "imessage", "+CONTACT_MINDY", "MINDY_UNIQUE_TOPIC", 1700000003);
    insert_feed(db, "imessage", "+CONTACT_BETTY", "BETTY_UNIQUE_TOPIC", 1700000002);
    insert_feed(db, "imessage", "+CONTACT_ANNIE", "ANNIE_UNIQUE_TOPIC", 1700000001);

    hu_contact_profile_t cp = {0};
    cp.contact_id = (char *)"+CONTACT_ANNIE";

    hu_feed_item_stored_t *out = NULL;
    size_t out_count = 0;
    HU_ASSERT_EQ(hu_daemon_proactive_get_contact_feed_items(&alloc, db, &cp, 32, &out, &out_count),
                 HU_OK);
    HU_ASSERT_EQ(out_count, 1u);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out[0].contact_id, "+CONTACT_ANNIE");
    HU_ASSERT_STR_EQ(out[0].content, "ANNIE_UNIQUE_TOPIC");

    /* Belt-and-suspenders: the other contacts' unique markers must NOT
     * appear in any returned row. With the old hu_feed_processor_get_all_recent
     * call site, all three rows would have been returned and these
     * substrings would have shown up. */
    for (size_t i = 0; i < out_count; i++) {
        HU_ASSERT_TRUE(strstr(out[i].content, "MINDY_UNIQUE_TOPIC") == NULL);
        HU_ASSERT_TRUE(strstr(out[i].content, "BETTY_UNIQUE_TOPIC") == NULL);
    }

    hu_feed_items_free(&alloc, out, out_count);
    mem.vtable->deinit(mem.ctx);
}

/* Unknown contact: no rows match → HU_OK with zero count, no allocation. */
static void test_get_contact_feed_items_returns_empty_for_unknown_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    insert_feed(db, "imessage", "+CONTACT_MINDY", "MINDY_UNIQUE_TOPIC", 1700000003);

    hu_contact_profile_t cp = {0};
    cp.contact_id = (char *)"+CONTACT_NONE";

    hu_feed_item_stored_t *out = NULL;
    size_t out_count = 0;
    HU_ASSERT_EQ(hu_daemon_proactive_get_contact_feed_items(&alloc, db, &cp, 32, &out, &out_count),
                 HU_OK);
    HU_ASSERT_EQ(out_count, 0u);
    HU_ASSERT_NULL(out);

    mem.vtable->deinit(mem.ctx);
}

/* Null out parameters → HU_ERR_INVALID_ARGUMENT. Pinning the argument-
 * validation contract so a future refactor doesn't silently drop it. */
static void test_get_contact_feed_items_rejects_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    hu_contact_profile_t cp = {.contact_id = (char *)"+CONTACT_A"};
    size_t out_count = 99;
    HU_ASSERT_EQ(hu_daemon_proactive_get_contact_feed_items(&alloc, db, &cp, 32, NULL, &out_count),
                 HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

/* Null cp or cp->contact_id → HU_ERR_INVALID_ARGUMENT. Without this
 * guard a caller could pass a partially-initialized contact profile and
 * the SQL would bind NULL to the contact_id parameter, returning zero
 * rows silently — a false-clean signal masking a config bug. */
static void test_get_contact_feed_items_rejects_null_cp_or_contact_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_feed_item_stored_t *out = NULL;
    size_t out_count = 0;
    HU_ASSERT_EQ(hu_daemon_proactive_get_contact_feed_items(&alloc, db, NULL, 32, &out, &out_count),
                 HU_ERR_INVALID_ARGUMENT);

    hu_contact_profile_t cp = {0}; /* contact_id == NULL */
    HU_ASSERT_EQ(hu_daemon_proactive_get_contact_feed_items(&alloc, db, &cp, 32, &out, &out_count),
                 HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

void run_daemon_proactive_feed_scope_tests(void) {
    HU_TEST_SUITE("daemon_proactive_feed_scope");
    HU_RUN_TEST(test_get_contact_feed_items_returns_only_target_contact);
    HU_RUN_TEST(test_get_contact_feed_items_returns_empty_for_unknown_contact);
    HU_RUN_TEST(test_get_contact_feed_items_rejects_null_out);
    HU_RUN_TEST(test_get_contact_feed_items_rejects_null_cp_or_contact_id);
}
