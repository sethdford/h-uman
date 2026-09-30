/* Exercises hu_reminder_repo_* in src/memory/repos/reminder_repo_sqlite.c
 * (life-admin slice 1): each reminder is claimed at most once per pass, a
 * crash mid-send is retried rather than lost, and one far past due is
 * recorded as missed instead of sent.
 * @covers-none — the production file is named above; "reminder_repo" has no
 * same-named source for the basename heuristic to find.
 */
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/reminder_repo.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>

#define OWNER "+15550000009"

static int64_t count_status(sqlite3 *db, const char *status) {
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM reminders WHERE status=?1;", -1, &st, NULL) !=
        SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void test_reminder_repo_claims_each_due_row_once(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    int64_t a = 0, b = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db, OWNER, "imessage", "call mom", 1100, 1000, "t", &a),
                 HU_OK);
    HU_ASSERT_EQ(hu_reminder_repo_add(db, OWNER, "imessage", "later thing", 5000, 1000, "t", &b),
                 HU_OK);
    HU_ASSERT_TRUE(a > 0 && b > a);

    hu_reminder_t rows[4];
    size_t n = 99, missed = 99;
    /* Pre: nothing due yet. */
    HU_ASSERT_EQ(hu_reminder_repo_claim_due(db, 1050, 7200, rows, 4, &n, &missed), HU_OK);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(missed, 0);

    HU_ASSERT_EQ(hu_reminder_repo_claim_due(db, 1100, 7200, rows, 4, &n, &missed), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(rows[0].id, a);
    HU_ASSERT_STR_EQ(rows[0].what, "call mom");
    HU_ASSERT_STR_EQ(rows[0].owner, OWNER);
    HU_ASSERT_STR_EQ(rows[0].channel, "imessage");
    HU_ASSERT_EQ(count_status(db, "sending"), 1);

    /* A second pass before the send finishes must not hand it out again. */
    HU_ASSERT_EQ(hu_reminder_repo_claim_due(db, 1120, 7200, rows, 4, &n, &missed), HU_OK);
    HU_ASSERT_EQ(n, 0);

    HU_ASSERT_EQ(hu_reminder_repo_mark(db, a, "sent", 1121), HU_OK);
    HU_ASSERT_EQ(count_status(db, "sent"), 1);
    HU_ASSERT_EQ(count_status(db, "pending"), 1);
    mem.vtable->deinit(mem.ctx);
}

static void test_reminder_repo_retries_a_send_orphaned_by_a_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db, OWNER, "imessage", "stretch", 1000, 900, "t", &id),
                 HU_OK);
    hu_reminder_t rows[2];
    size_t n = 0;
    HU_ASSERT_EQ(hu_reminder_repo_claim_due(db, 1000, 7200, rows, 2, &n, NULL), HU_OK);
    HU_ASSERT_EQ(n, 1);
    /* No mark: the process died mid-send. Inside 10 minutes it stays claimed… */
    HU_ASSERT_EQ(hu_reminder_repo_claim_due(db, 1500, 7200, rows, 2, &n, NULL), HU_OK);
    HU_ASSERT_EQ(n, 0);
    /* …after that it is handed out again rather than lost. */
    HU_ASSERT_EQ(hu_reminder_repo_claim_due(db, 1700, 7200, rows, 2, &n, NULL), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(rows[0].id, id);
    mem.vtable->deinit(mem.ctx);
}

static void test_reminder_repo_marks_long_overdue_rows_missed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db, OWNER, "imessage", "old", 1000, 900, "t", &id), HU_OK);
    HU_ASSERT_EQ(hu_reminder_repo_add(db, OWNER, "imessage", "fresh", 9000, 900, "t", &id), HU_OK);
    hu_reminder_t rows[2];
    size_t n = 0, missed = 0;
    HU_ASSERT_EQ(hu_reminder_repo_claim_due(db, 9100, 7200, rows, 2, &n, &missed), HU_OK);
    HU_ASSERT_EQ(missed, 1);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_EQ(rows[0].what, "fresh");
    HU_ASSERT_EQ(count_status(db, "missed"), 1);

    hu_reminder_t m[2];
    HU_ASSERT_EQ(hu_reminder_repo_missed(db, m, 2, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_EQ(m[0].what, "old");
    HU_ASSERT_EQ(hu_reminder_repo_mark(db, m[0].id, "missed_told", 9200), HU_OK);
    HU_ASSERT_EQ(hu_reminder_repo_missed(db, m, 2, &n), HU_OK);
    HU_ASSERT_EQ(n, 0); /* told once, not again */
    mem.vtable->deinit(mem.ctx);
}

static void test_reminder_repo_last_sent_snooze_and_upcoming(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    hu_reminder_t last;
    HU_ASSERT_EQ(hu_reminder_repo_last_sent(db, OWNER, 0, &last), HU_ERR_NOT_FOUND);

    int64_t id = 0, other = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db, OWNER, "imessage", "pay rent", 1000, 900, "t", &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_reminder_repo_add(db, OWNER, "imessage", "dentist", 3000, 900, "t", &other),
                 HU_OK);
    hu_reminder_t rows[2];
    size_t n = 0;
    HU_ASSERT_EQ(hu_reminder_repo_claim_due(db, 1000, 7200, rows, 2, &n, NULL), HU_OK);
    HU_ASSERT_EQ(hu_reminder_repo_mark(db, id, "sent", 1001), HU_OK);

    HU_ASSERT_EQ(hu_reminder_repo_last_sent(db, OWNER, 1000, &last), HU_OK);
    HU_ASSERT_EQ(last.id, id);
    /* Outside the window, or for someone else, there is nothing to act on. */
    HU_ASSERT_EQ(hu_reminder_repo_last_sent(db, OWNER, 1002, &last), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_reminder_repo_last_sent(db, "+15550000001", 0, &last), HU_ERR_NOT_FOUND);

    HU_ASSERT_EQ(hu_reminder_repo_snooze(db, id, 4000, 1010), HU_OK);
    hu_reminder_t up[4];
    HU_ASSERT_EQ(hu_reminder_repo_upcoming(db, OWNER, up, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 2);
    HU_ASSERT_STR_EQ(up[0].what, "dentist"); /* soonest first */
    HU_ASSERT_STR_EQ(up[1].what, "pay rent");
    HU_ASSERT_EQ(up[1].due_at, 4000);
    mem.vtable->deinit(mem.ctx);
}

static void test_reminder_repo_rejects_bad_arguments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    int64_t id = 0;
    hu_reminder_t rows[1];
    size_t n = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db, OWNER, "imessage", "", 1, 1, "t", &id),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_reminder_repo_add(NULL, OWNER, "imessage", "x", 1, 1, "t", &id),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_reminder_repo_claim_due(db, 1, 0, rows, 1, &n, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_reminder_repo_add(db, OWNER, "imessage", "x", 10, 1, "t", &id), HU_OK);
    /* Only the three documented transitions are accepted. */
    HU_ASSERT_EQ(hu_reminder_repo_mark(db, id, "missed", 2), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_reminder_repo_mark(db, id, "sending", 2), HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

void run_reminder_repo_tests(void) {
    HU_TEST_SUITE("reminder repo");
    HU_RUN_TEST(test_reminder_repo_claims_each_due_row_once);
    HU_RUN_TEST(test_reminder_repo_retries_a_send_orphaned_by_a_crash);
    HU_RUN_TEST(test_reminder_repo_marks_long_overdue_rows_missed);
    HU_RUN_TEST(test_reminder_repo_last_sent_snooze_and_upcoming);
    HU_RUN_TEST(test_reminder_repo_rejects_bad_arguments);
}
#else
void run_reminder_repo_tests(void) {}
#endif
