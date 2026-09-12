/* tests/test_prospective.c
 *
 * hu_prospective_check_triggers (src/memory/prospective.c) — the read side of
 * prospective memory. Rows are written by scripts/insight_stream.py
 * (--prospective), so the fixture seeds prospective_memories directly, the way
 * the daemon finds them. Pins: case-folded substring match on the inbound
 * text (the case-sensitive form never fired on real messages), per-contact
 * scoping, fired rows excluded, expired rows excluded. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory/engines.h"
#include "human/memory/prospective.h"

#include <sqlite3.h>
#include <string.h>
#include <time.h>

static void seed(sqlite3 *db, const char *type, const char *value, const char *action,
                 const char *contact, int64_t expires_at, int fired) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('%s','%s','%s',%s%s%s,%lld,%d,%lld)",
             type, value, action, contact ? "'" : "", contact ? contact : "NULL",
             contact ? "'" : "", (long long)expires_at, fired, (long long)time(NULL));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

static void check_triggers_matches_case_folded_substring_for_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    seed(db, "keyword", "github invite", "resend the github invite", "+15550000001", 0, 0);
    seed(db, "keyword", "podcast", "send podcast recs", "+15550000002", 0, 0); /* other contact */

    hu_prospective_entry_t *out = NULL;
    size_t n = 0;
    static const char msg[] = "Did you resend the GitHub Invite?";
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "keyword", msg, sizeof(msg) - 1,
                                               "+15550000001", 12, (int64_t)time(NULL), &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)1);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out[0].action, "resend the github invite");
    HU_ASSERT_STR_EQ(out[0].trigger_value, "github invite");
    alloc.free(alloc.ctx, out, n * sizeof(hu_prospective_entry_t));
    mem.vtable->deinit(mem.ctx);
}

static void check_triggers_skips_fired_and_expired_rows(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    int64_t now = (int64_t)time(NULL);
    seed(db, "keyword", "blinds", "ask about the blinds", NULL, 0, 1);              /* fired */
    seed(db, "keyword", "blinds", "ask about the blinds again", NULL, now - 60, 0); /* expired */
    seed(db, "keyword", "blinds", "still open", NULL, now + 3600, 0);               /* live */

    hu_prospective_entry_t *out = NULL;
    size_t n = 0;
    static const char msg[] = "the blinds guy came by";
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "keyword", msg, sizeof(msg) - 1, NULL, 0,
                                               now, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)1);
    HU_ASSERT_STR_EQ(out[0].action, "still open");
    alloc.free(alloc.ctx, out, n * sizeof(hu_prospective_entry_t));

    /* nothing matching → HU_OK with an empty result, never a stale pointer */
    out = (hu_prospective_entry_t *)0x1;
    n = 99;
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "keyword", "unrelated", 9, NULL, 0, now,
                                               &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)0);
    HU_ASSERT_NULL(out);
    mem.vtable->deinit(mem.ctx);
}

void run_prospective_tests(void) {
    HU_TEST_SUITE("prospective memory triggers");
    HU_RUN_TEST(check_triggers_matches_case_folded_substring_for_contact);
    HU_RUN_TEST(check_triggers_skips_fired_and_expired_rows);
}

#else

void run_prospective_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
