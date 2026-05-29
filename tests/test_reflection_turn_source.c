/* tests/test_reflection_turn_source.c — T9-followup production turn source.
 *
 * Pins the contract of hu_reflection_sqlite_turn_source_*, the real
 * turn iterator that replaces stub_turn_iter in
 * src/daemon_reflection_tick.c. It reads the canonical `messages`
 * conversation ledger (written by the memory engine's save_message
 * path) and adapts each row to hu_reflection_turn_t.
 *
 * Contracts:
 *   AC-1 init+iter yields the messages OLDEST-FIRST with correct field
 *        mapping (turn_id="msg-<id>", channel=session_id, sender=role,
 *        content=content, ts_ms = created_at->epoch-ms).
 *   AC-2 max_turns caps the result to the N MOST-RECENT turns (still
 *        presented oldest-first).
 *   AC-3 empty messages table -> iter returns false on first call
 *        (drives hu_reflection_run to NO_INPUT, same as the old stub).
 *   AC-4 max_turns <= 0 falls back to the default cap.
 *   AC-5 NULL ctx / NULL out_turn are safe (return false).
 *   AC-6 the source feeds hu_reflection_build_input cleanly end-to-end.
 *
 * Fixture: in-memory SQLite with the `messages` table created to match
 * src/memory/engines/sqlite.c exactly. */

#include "human/reflection.h"
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* messages schema copied verbatim from src/memory/engines/sqlite.c so
 * the turn source reads exactly what the daemon writes in production. */
static void create_messages_table(sqlite3 *db) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db,
                          "CREATE TABLE IF NOT EXISTS messages("
                          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                          "session_id TEXT NOT NULL,role TEXT NOT NULL,"
                          "content TEXT NOT NULL,created_at TEXT DEFAULT(datetime('now')))",
                          NULL, NULL, &errmsg);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    if (errmsg)
        sqlite3_free(errmsg);
}

/* Insert one message row with an explicit created_at so the ts_ms
 * conversion is deterministic. */
static void insert_message(sqlite3 *db, const char *session_id, const char *role,
                           const char *content, const char *created_at) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO messages (session_id, role, content, created_at) VALUES (?1,?2,?3,?4)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    sqlite3_bind_text(st, 1, session_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, role, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, content, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, created_at, -1, SQLITE_STATIC);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
}

/* Compute the epoch-ms the source SHOULD report for a given created_at,
 * using the same strftime the implementation uses. */
static uint64_t expected_ts_ms(sqlite3 *db, const char *created_at) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(
        sqlite3_prepare_v2(db, "SELECT CAST(strftime('%s', ?1) AS INTEGER) * 1000", -1, &st, NULL),
        SQLITE_OK);
    sqlite3_bind_text(st, 1, created_at, -1, SQLITE_STATIC);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    uint64_t ms = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return ms;
}

static sqlite3 *open_mem_db(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_NOT_NULL(db);
    return db;
}

/* AC-1: oldest-first ordering + full field mapping. */
static void turn_source_yields_messages_oldest_first(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_mem_db();
    create_messages_table(db);

    insert_message(db, "imessage", "user", "hey what's up", "2026-05-01 09:00:00");
    insert_message(db, "imessage", "assistant", "not much, you?", "2026-05-01 09:01:00");
    insert_message(db, "telegram", "user", "ping", "2026-05-01 10:00:00");

    hu_reflection_sqlite_turn_source_t *src = NULL;
    HU_ASSERT_EQ(hu_reflection_sqlite_turn_source_init(&src, db, &alloc, 100), HU_OK);
    HU_ASSERT_NOT_NULL(src);

    hu_reflection_turn_t t;

    /* row 1 (oldest) */
    HU_ASSERT_TRUE(hu_reflection_sqlite_turn_iter(src, &t));
    HU_ASSERT_STR_EQ(t.turn_id, "msg-1");
    HU_ASSERT_STR_EQ(t.channel, "imessage");
    HU_ASSERT_STR_EQ(t.sender, "user");
    HU_ASSERT_STR_EQ(t.content, "hey what's up");
    HU_ASSERT_EQ((long long)t.ts_ms, (long long)expected_ts_ms(db, "2026-05-01 09:00:00"));

    /* row 2 */
    HU_ASSERT_TRUE(hu_reflection_sqlite_turn_iter(src, &t));
    HU_ASSERT_STR_EQ(t.turn_id, "msg-2");
    HU_ASSERT_STR_EQ(t.sender, "assistant");
    HU_ASSERT_STR_EQ(t.content, "not much, you?");

    /* row 3 (newest) */
    HU_ASSERT_TRUE(hu_reflection_sqlite_turn_iter(src, &t));
    HU_ASSERT_STR_EQ(t.turn_id, "msg-3");
    HU_ASSERT_STR_EQ(t.channel, "telegram");

    /* end of stream */
    HU_ASSERT_FALSE(hu_reflection_sqlite_turn_iter(src, &t));
    /* idempotent at end */
    HU_ASSERT_FALSE(hu_reflection_sqlite_turn_iter(src, &t));

    hu_reflection_sqlite_turn_source_dispose(src);
    sqlite3_close(db);
}

/* AC-2: max_turns keeps only the N most-recent, still oldest-first. */
static void turn_source_caps_to_most_recent_n(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_mem_db();
    create_messages_table(db);

    insert_message(db, "s", "user", "m1", "2026-05-01 09:00:00");
    insert_message(db, "s", "user", "m2", "2026-05-01 09:01:00");
    insert_message(db, "s", "user", "m3", "2026-05-01 09:02:00");
    insert_message(db, "s", "user", "m4", "2026-05-01 09:03:00");

    hu_reflection_sqlite_turn_source_t *src = NULL;
    HU_ASSERT_EQ(hu_reflection_sqlite_turn_source_init(&src, db, &alloc, 2), HU_OK);

    hu_reflection_turn_t t;
    /* most-recent 2 rows are m3,m4 — presented oldest-first → m3 then m4 */
    HU_ASSERT_TRUE(hu_reflection_sqlite_turn_iter(src, &t));
    HU_ASSERT_STR_EQ(t.content, "m3");
    HU_ASSERT_STR_EQ(t.turn_id, "msg-3");
    HU_ASSERT_TRUE(hu_reflection_sqlite_turn_iter(src, &t));
    HU_ASSERT_STR_EQ(t.content, "m4");
    HU_ASSERT_STR_EQ(t.turn_id, "msg-4");
    HU_ASSERT_FALSE(hu_reflection_sqlite_turn_iter(src, &t));

    hu_reflection_sqlite_turn_source_dispose(src);
    sqlite3_close(db);
}

/* AC-3: empty ledger → iter false immediately (NO_INPUT path). */
static void turn_source_empty_table_yields_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_mem_db();
    create_messages_table(db);

    hu_reflection_sqlite_turn_source_t *src = NULL;
    HU_ASSERT_EQ(hu_reflection_sqlite_turn_source_init(&src, db, &alloc, 100), HU_OK);

    hu_reflection_turn_t t;
    HU_ASSERT_FALSE(hu_reflection_sqlite_turn_iter(src, &t));

    hu_reflection_sqlite_turn_source_dispose(src);
    sqlite3_close(db);
}

/* AC-4: max_turns <= 0 falls back to the default cap (does not error,
 * does not return zero rows). */
static void turn_source_nonpositive_max_uses_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_mem_db();
    create_messages_table(db);
    insert_message(db, "s", "user", "only", "2026-05-01 09:00:00");

    hu_reflection_sqlite_turn_source_t *src = NULL;
    HU_ASSERT_EQ(hu_reflection_sqlite_turn_source_init(&src, db, &alloc, 0), HU_OK);

    hu_reflection_turn_t t;
    HU_ASSERT_TRUE(hu_reflection_sqlite_turn_iter(src, &t));
    HU_ASSERT_STR_EQ(t.content, "only");
    HU_ASSERT_FALSE(hu_reflection_sqlite_turn_iter(src, &t));

    hu_reflection_sqlite_turn_source_dispose(src);
    sqlite3_close(db);
}

/* AC-5: NULL-safety. */
static void turn_source_null_args_are_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_mem_db();
    create_messages_table(db);
    insert_message(db, "s", "user", "x", "2026-05-01 09:00:00");

    /* NULL out_src / db / alloc → INVALID_ARGUMENT, no crash. */
    HU_ASSERT_EQ(hu_reflection_sqlite_turn_source_init(NULL, db, &alloc, 10),
                 HU_ERR_INVALID_ARGUMENT);
    hu_reflection_sqlite_turn_source_t *src = NULL;
    HU_ASSERT_EQ(hu_reflection_sqlite_turn_source_init(&src, NULL, &alloc, 10),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_reflection_sqlite_turn_source_init(&src, db, NULL, 10),
                 HU_ERR_INVALID_ARGUMENT);

    /* NULL ctx / out_turn on the iter → false, no crash. */
    hu_reflection_turn_t t;
    HU_ASSERT_FALSE(hu_reflection_sqlite_turn_iter(NULL, &t));
    HU_ASSERT_EQ(hu_reflection_sqlite_turn_source_init(&src, db, &alloc, 10), HU_OK);
    HU_ASSERT_FALSE(hu_reflection_sqlite_turn_iter(src, NULL));

    /* dispose(NULL) is a no-op. */
    hu_reflection_sqlite_turn_source_dispose(NULL);

    hu_reflection_sqlite_turn_source_dispose(src);
    sqlite3_close(db);
}

/* AC-6: end-to-end through hu_reflection_build_input — the real
 * consumer of the iter contract. */
static void turn_source_feeds_build_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_mem_db();
    create_messages_table(db);
    insert_message(db, "imessage", "user", "first", "2026-05-01 09:00:00");
    insert_message(db, "imessage", "assistant", "second", "2026-05-01 09:01:00");

    hu_reflection_sqlite_turn_source_t *src = NULL;
    HU_ASSERT_EQ(hu_reflection_sqlite_turn_source_init(&src, db, &alloc, 100), HU_OK);

    char *body = NULL;
    int turn_count = 0;
    HU_ASSERT_EQ(hu_reflection_build_input(hu_reflection_sqlite_turn_iter, src, /*max_chars=*/0,
                                           &body, &turn_count),
                 HU_OK);
    HU_ASSERT_NOT_NULL(body);
    HU_ASSERT_EQ(turn_count, 2);
    /* Both messages should appear in the assembled transcript body. */
    HU_ASSERT_TRUE(strstr(body, "first") != NULL);
    HU_ASSERT_TRUE(strstr(body, "second") != NULL);
    free(body);

    hu_reflection_sqlite_turn_source_dispose(src);
    sqlite3_close(db);
}

void run_reflection_turn_source_tests(void) {
    HU_TEST_SUITE("reflection_turn_source");
    HU_RUN_TEST(turn_source_yields_messages_oldest_first);
    HU_RUN_TEST(turn_source_caps_to_most_recent_n);
    HU_RUN_TEST(turn_source_empty_table_yields_nothing);
    HU_RUN_TEST(turn_source_nonpositive_max_uses_default);
    HU_RUN_TEST(turn_source_null_args_are_safe);
    HU_RUN_TEST(turn_source_feeds_build_input);
}

#else /* !HU_ENABLE_SQLITE */

void run_reflection_turn_source_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
