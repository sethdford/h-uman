/* tests/test_outbound_crosstalk_sqlite.c
 *
 * Sprint 60 follow-up to Sprint 59 — wire the outbound crosstalk stage's
 * cross-contact bleed check to the production `messages` table. Closes
 * the "degraded mode" gap noted at
 * docs/plans/2026-05-26-sprint-59-outbound-safety/STATUS.md item #3.
 *
 * Pins the contract of:
 *   - hu_outbound_crosstalk_sqlite_lookup — the lookup function
 *     itself: SELECT content FROM messages WHERE session_id != ?
 *     AND created_at > datetime('now', '-7 days') with proper
 *     allocator-owned-strings contract per outbound_pipeline.h.
 *   - hu_outbound_crosstalk_register_sqlite / _unregister_sqlite —
 *     the daemon-side install/uninstall wrappers.
 *
 * Gated on HU_ENABLE_SQLITE (the helper is SQLite-gated).
 */

#include "test_framework.h"

#include "human/agent/outbound_crosstalk_sqlite.h"
#include "human/agent/outbound_pipeline.h"
#include "human/core/allocator.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_crosstalk;

static void run_sql_or_fail(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    if (err)
        sqlite3_free(err);
}

static sqlite3 *open_test_db(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    /* Match the schema in src/memory/engines/sqlite.c:59. */
    run_sql_or_fail(db, "CREATE TABLE messages("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        "session_id TEXT NOT NULL,"
                        "role TEXT NOT NULL,"
                        "content TEXT NOT NULL,"
                        "created_at TEXT DEFAULT(datetime('now')))");
    return db;
}

static void free_lookup_result(hu_allocator_t *alloc, char **arr, size_t count) {
    if (!arr)
        return;
    for (size_t i = 0; i < count; i++)
        if (arr[i])
            alloc->free(alloc->ctx, arr[i], strlen(arr[i]) + 1);
    alloc->free(alloc->ctx, arr, count * sizeof(char *));
}

/* ── Lookup contract ──────────────────────────────────────────────────── */

/* The load-bearing scope test: lookup must return content from contacts
 * OTHER than the recipient, and never the recipient's own content. This
 * mirrors the Annie/Mindy/Betty regression on the egress side — if a
 * stale row keyed to Annie made it to the moment Mindy was being
 * messaged, the crosstalk stage should catch it. */
static void test_lookup_returns_other_contacts_only(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();

    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_A', 'assistant', 'ALPHA_MARKER_AAA', "
                        "datetime('now'))");
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_B', 'assistant', 'BRAVO_MARKER_BBB', "
                        "datetime('now'))");
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_C', 'assistant', 'CHARLIE_MARKER_CCC', "
                        "datetime('now'))");

    char **out = NULL;
    size_t count = 0;
    int rc = hu_outbound_crosstalk_sqlite_lookup(db, &alloc, "+CONTACT_A", strlen("+CONTACT_A"),
                                                 &out, &count);
    HU_ASSERT_EQ(rc, 0);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(out);

    bool saw_a = false, saw_b = false, saw_c = false;
    for (size_t i = 0; i < count; i++) {
        if (strstr(out[i], "ALPHA_MARKER_AAA"))
            saw_a = true;
        if (strstr(out[i], "BRAVO_MARKER_BBB"))
            saw_b = true;
        if (strstr(out[i], "CHARLIE_MARKER_CCC"))
            saw_c = true;
    }
    HU_ASSERT_FALSE(saw_a); /* recipient excluded */
    HU_ASSERT_TRUE(saw_b);
    HU_ASSERT_TRUE(saw_c);

    free_lookup_result(&alloc, out, count);
    sqlite3_close(db);
}

/* 7-day filter per spec — messages older than the window are not
 * returned. Without this filter the cross-contact pool grows unbounded
 * with the daemon's lifetime, and old phrasing from months ago would
 * trigger spurious REJECTs on unrelated current messages. */
static void test_lookup_filters_messages_older_than_7_days(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();

    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_OTHER', 'assistant', 'ANCIENT_MARKER', "
                        "datetime('now', '-10 days'))");
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_OTHER', 'assistant', 'FRESH_MARKER', "
                        "datetime('now'))");

    char **out = NULL;
    size_t count = 0;
    int rc = hu_outbound_crosstalk_sqlite_lookup(db, &alloc, "+CONTACT_RECIPIENT",
                                                 strlen("+CONTACT_RECIPIENT"), &out, &count);
    HU_ASSERT_EQ(rc, 0);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out[0], "FRESH_MARKER") != NULL);
    HU_ASSERT_TRUE(strstr(out[0], "ANCIENT_MARKER") == NULL);

    free_lookup_result(&alloc, out, count);
    sqlite3_close(db);
}

/* Empty result: zero rows for any other contact must yield count=0 with
 * NULL out_texts (no allocation). Otherwise the stage's free path frees
 * an empty allocation — wasteful but not a leak, just sloppy.
 * Matching the fake_lookup convention in test_outbound_crosstalk.c. */
static void test_lookup_empty_returns_zero_with_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();

    char **out = NULL;
    size_t count = 0;
    int rc = hu_outbound_crosstalk_sqlite_lookup(db, &alloc, "+CONTACT_A", strlen("+CONTACT_A"),
                                                 &out, &count);
    HU_ASSERT_EQ(rc, 0);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(out);

    sqlite3_close(db);
}

/* Argument validation — every NULL arg returns -1 without touching the
 * out parameters. Without this guard a misconfigured daemon could
 * crash the outbound path; with it, the stage falls to graceful
 * degraded SEND. */
static void test_lookup_null_args_return_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();

    char **out = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_outbound_crosstalk_sqlite_lookup(NULL, &alloc, "+A", 2, &out, &count), -1);
    HU_ASSERT_EQ(hu_outbound_crosstalk_sqlite_lookup(db, NULL, "+A", 2, &out, &count), -1);
    HU_ASSERT_EQ(hu_outbound_crosstalk_sqlite_lookup(db, &alloc, NULL, 0, &out, &count), -1);
    HU_ASSERT_EQ(hu_outbound_crosstalk_sqlite_lookup(db, &alloc, "+A", 2, NULL, &count), -1);
    HU_ASSERT_EQ(hu_outbound_crosstalk_sqlite_lookup(db, &alloc, "+A", 2, &out, NULL), -1);

    sqlite3_close(db);
}

/* ── Register / unregister wrappers ───────────────────────────────────── */

/* After register, the crosstalk stage routes through this lookup against
 * the supplied db. Feed it a verbatim copy of another contact's existing
 * message and the stage must REJECT with the crosstalk reason. After
 * unregister, the stage falls back to the degraded SEND path. This is
 * the END-TO-END contract that closes STATUS.md item #3. */
static void test_register_then_stage_rejects_crosstalk(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_OTHER', 'assistant', "
                        "'but boy I am just more lonely now than ever', "
                        "datetime('now'))");

    hu_outbound_crosstalk_register_sqlite(db);

    hu_outbound_message_t msg = {0};
    /* Verbatim copy of the other contact's content — Jaccard 1.0,
     * must REJECT. */
    msg.content = (char *)"but boy I am just more lonely now than ever";
    msg.content_len = strlen(msg.content);
    hu_outbound_context_t ctx = {0};
    ctx.alloc = &alloc;
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.recipient_contact_id = "+CONTACT_RECIPIENT";
    ctx.recipient_contact_id_len = strlen("+CONTACT_RECIPIENT");

    hu_outbound_verdict_t v =
        hu_outbound_pipeline_stage_crosstalk.run(&hu_outbound_pipeline_stage_crosstalk, &msg, &ctx);
    HU_ASSERT_TRUE(v.kind == HU_OUTBOUND_REJECT);
    HU_ASSERT_NOT_NULL(v.reason);
    HU_ASSERT_TRUE(strstr(v.reason, "crosstalk_other_contact") != NULL);

    hu_outbound_crosstalk_unregister_sqlite();

    /* After unregister, the same content goes to SEND (degraded mode,
     * no cross-contact corpus available). */
    hu_outbound_verdict_t v2 =
        hu_outbound_pipeline_stage_crosstalk.run(&hu_outbound_pipeline_stage_crosstalk, &msg, &ctx);
    HU_ASSERT_TRUE(v2.kind == HU_OUTBOUND_SEND);

    sqlite3_close(db);
}

/* Lifecycle hygiene: register(NULL) is a no-op equivalent to
 * unregister — guards against accidental crashes when the daemon's
 * sqlite handle isn't open yet. */
static void test_register_null_is_equivalent_to_unregister(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_OTHER', 'assistant', "
                        "'but boy I am just more lonely now than ever', "
                        "datetime('now'))");

    hu_outbound_crosstalk_register_sqlite(db);
    hu_outbound_crosstalk_register_sqlite(NULL); /* should clear */

    hu_outbound_message_t msg = {0};
    msg.content = (char *)"but boy I am just more lonely now than ever";
    msg.content_len = strlen(msg.content);
    hu_outbound_context_t ctx = {0};
    ctx.alloc = &alloc;
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.recipient_contact_id = "+CONTACT_RECIPIENT";
    ctx.recipient_contact_id_len = strlen("+CONTACT_RECIPIENT");

    hu_outbound_verdict_t v =
        hu_outbound_pipeline_stage_crosstalk.run(&hu_outbound_pipeline_stage_crosstalk, &msg, &ctx);
    HU_ASSERT_TRUE(v.kind == HU_OUTBOUND_SEND); /* degraded; lookup was cleared */

    hu_outbound_crosstalk_unregister_sqlite();
    sqlite3_close(db);
}

void run_outbound_crosstalk_sqlite_tests(void) {
    HU_TEST_SUITE("outbound_crosstalk_sqlite");
    HU_RUN_TEST(test_lookup_returns_other_contacts_only);
    HU_RUN_TEST(test_lookup_filters_messages_older_than_7_days);
    HU_RUN_TEST(test_lookup_empty_returns_zero_with_null_out);
    HU_RUN_TEST(test_lookup_null_args_return_error);
    HU_RUN_TEST(test_register_then_stage_rejects_crosstalk);
    HU_RUN_TEST(test_register_null_is_equivalent_to_unregister);
    /* Defensive — leave global state clean for other suites. */
    hu_outbound_crosstalk_unregister_sqlite();
}
