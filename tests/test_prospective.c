/* tests/test_prospective.c
 *
 * hu_prospective_check_triggers + hu_prospective_mark_fired
 * (src/memory/prospective.c) — the read side of prospective memory. Rows are
 * written by scripts/insight_stream.py (--prospective), so the fixture seeds
 * prospective_memories directly, the way the daemon finds them. Pins:
 * case-folded WHOLE-WORD match on the inbound text (the substring form fired
 * "work" on "bath and body works"; the case-sensitive form never fired at
 * all), per-contact scoping, fired rows excluded, expired rows excluded,
 * newest intention first, one row per intention even when two of its
 * keywords appear, and mark_fired retiring every keyword of the intention
 * (2026-09-13: 949 live rows, 0 ever fired, nothing set fired=1). */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory/engines.h"
#include "human/memory/prospective.h"

#include <sqlite3.h>
#include <string.h>
#include <time.h>

static void seed_at(sqlite3 *db, const char *type, const char *value, const char *action,
                    const char *contact, int64_t expires_at, int fired, int64_t created_at) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('%s','%s','%s',%s%s%s,%lld,%d,%lld)",
             type, value, action, contact ? "'" : "", contact ? contact : "NULL",
             contact ? "'" : "", (long long)expires_at, fired, (long long)created_at);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

static void seed(sqlite3 *db, const char *type, const char *value, const char *action,
                 const char *contact, int64_t expires_at, int fired) {
    seed_at(db, type, value, action, contact, expires_at, fired, (int64_t)time(NULL));
}

static int64_t count_open_for(sqlite3 *db, const char *contact) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM prospective_memories WHERE fired=0 AND contact_id='%s'",
             contact);
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void check_triggers_matches_case_folded_phrase_for_contact(void) {
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

/* Live 2026-09-12: the keyword "work" (18 open rows) fired on "Ru a bath and
 * body works kinda guy?" through instr(). A cue is a word, not a byte run. */
static void check_triggers_matches_whole_words_not_substrings(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    seed(db, "keyword", "work", "ask about their internship", "+15550000001", 0, 0);
    int64_t now = (int64_t)time(NULL);

    hu_prospective_entry_t *out = NULL;
    size_t n = 0;
    static const char inside[] = "Ru a bath and body works kinda guy?";
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "keyword", inside, sizeof(inside) - 1,
                                               "+15550000001", 12, now, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)0);
    HU_ASSERT_NULL(out);

    static const char whole[] = "I would have to take off work, rn i cant";
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "keyword", whole, sizeof(whole) - 1,
                                               "+15550000001", 12, now, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)1);
    HU_ASSERT_STR_EQ(out[0].action, "ask about their internship");
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

/* The daemon renders at most 3 entries. Without an order the SELECT handed it
 * the three OLDEST rows forever; and an intention with two matching keywords
 * took two of the three slots for one reminder. */
static void check_triggers_newest_first_and_one_row_per_intention(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    int64_t now = (int64_t)time(NULL);
    seed_at(db, "keyword", "flight", "ask about flight home", "+15550000001", 0, 0, now - 86400);
    seed_at(db, "keyword", "dyson", "ask about dyson charger", "+15550000001", 0, 0, now - 60);
    seed_at(db, "keyword", "charger", "ask about dyson charger", "+15550000001", 0, 0, now - 60);

    hu_prospective_entry_t *out = NULL;
    size_t n = 0;
    static const char msg[] = "flight was fine, the dyson charger showed up";
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "keyword", msg, sizeof(msg) - 1,
                                               "+15550000001", 12, now, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)2);
    HU_ASSERT_STR_EQ(out[0].action, "ask about dyson charger"); /* newest intention first */
    HU_ASSERT_STR_EQ(out[1].action, "ask about flight home");
    alloc.free(alloc.ctx, out, n * sizeof(hu_prospective_entry_t));
    mem.vtable->deinit(mem.ctx);
}

static void mark_fired_retires_every_keyword_of_the_intention(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    int64_t now = (int64_t)time(NULL);
    seed(db, "keyword", "dyson", "ask about dyson charger", "+15550000001", 0, 0);
    seed(db, "keyword", "charger", "ask about dyson charger", "+15550000001", 0, 0);
    seed(db, "keyword", "dyson", "ask about their dyson", "+15550000002", 0, 0); /* other contact */
    HU_ASSERT_EQ(count_open_for(db, "+15550000001"), (int64_t)2);

    hu_prospective_entry_t *out = NULL;
    size_t n = 0;
    static const char msg[] = "the dyson came";
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "keyword", msg, sizeof(msg) - 1,
                                               "+15550000001", 12, now, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)1);
    HU_ASSERT_EQ(hu_prospective_mark_fired(db, out, n), HU_OK);
    alloc.free(alloc.ctx, out, n * sizeof(hu_prospective_entry_t));

    /* both keyword rows of the surfaced intention are retired; the other
     * contact's identical keyword is untouched */
    HU_ASSERT_EQ(count_open_for(db, "+15550000001"), (int64_t)0);
    HU_ASSERT_EQ(count_open_for(db, "+15550000002"), (int64_t)1);

    /* and the same cue no longer re-fires for this contact */
    static const char again[] = "charger works now";
    HU_ASSERT_EQ(hu_prospective_check_triggers(&alloc, db, "keyword", again, sizeof(again) - 1,
                                               "+15550000001", 12, now, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)0);
    HU_ASSERT_NULL(out);

    /* contract: NULL db / entries with a count is invalid; zero entries is a no-op */
    HU_ASSERT_EQ(hu_prospective_mark_fired(NULL, NULL, 0), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_prospective_mark_fired(db, NULL, 1), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_prospective_mark_fired(db, NULL, 0), HU_OK);
    mem.vtable->deinit(mem.ctx);
}

void run_prospective_tests(void) {
    HU_TEST_SUITE("prospective memory triggers");
    HU_RUN_TEST(check_triggers_matches_case_folded_phrase_for_contact);
    HU_RUN_TEST(check_triggers_matches_whole_words_not_substrings);
    HU_RUN_TEST(check_triggers_skips_fired_and_expired_rows);
    HU_RUN_TEST(check_triggers_newest_first_and_one_row_per_intention);
    HU_RUN_TEST(mark_fired_retires_every_keyword_of_the_intention);
}

#else

void run_prospective_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
