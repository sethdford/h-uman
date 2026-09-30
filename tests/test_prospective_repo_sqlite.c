/* tests/test_prospective_repo_sqlite.c
 *
 * Prospective memory v2 typed store (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1).
 * Task 1 pins the additive migration: v2 columns on a pre-v2 table, the
 * fired -> status mapping for existing rows, the trigger that keeps status in
 * step when a legacy writer (the nightly curator) changes fired, idempotence,
 * and that every engine-opened database is migrated. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/prospective_policy.h"
#include "human/memory/prospective_repo.h"
#include "human/memory/superhuman.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* The table exactly as src/memory/engines/sqlite.c created it before v2. */
static sqlite3 *legacy_db(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY "
                              "AUTOINCREMENT,trigger_type TEXT NOT NULL,trigger_value TEXT NOT "
                              "NULL,action TEXT NOT NULL,contact_id TEXT,expires_at INTEGER,fired "
                              "INTEGER DEFAULT 0,created_at INTEGER NOT NULL)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    return db;
}

static void legacy_insert(sqlite3 *db, const char *action, int fired) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('keyword','kw','%s','+15550000001',0,%d,100)",
             action, fired);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

static int64_t q_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void q_text(sqlite3 *db, const char *sql, char *buf, size_t cap) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    const unsigned char *t = sqlite3_column_text(st, 0);
    snprintf(buf, cap, "%s", t ? (const char *)t : "(null)");
    sqlite3_finalize(st);
}

static int64_t column_count(sqlite3 *db) {
    return q_int(db, "SELECT COUNT(*) FROM pragma_table_info('prospective_memories')");
}

static void ensure_schema_adds_typed_columns_and_maps_fired(void) {
    sqlite3 *db = legacy_db();
    legacy_insert(db, "open", 0);
    legacy_insert(db, "fired", 1);
    legacy_insert(db, "pruned", 2);
    legacy_insert(db, "expired", 3);
    HU_ASSERT_EQ(column_count(db), (int64_t)8);

    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);

    HU_ASSERT_EQ(column_count(db), (int64_t)15);
    static const char *const cols[] = {"cue_kind", "due_at",  "status", "surfaced_at",
                                       "attempts", "outcome", "source"};
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        char sql[160];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM pragma_table_info('prospective_memories') WHERE name='%s'",
                 cols[i]);
        HU_ASSERT_EQ(q_int(db, sql), (int64_t)1);
    }
    char s[32];
    q_text(db, "SELECT status FROM prospective_memories WHERE action='open'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending");
    q_text(db, "SELECT status FROM prospective_memories WHERE action='fired'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "done");
    q_text(db, "SELECT status FROM prospective_memories WHERE action='pruned'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "canceled");
    q_text(db, "SELECT status FROM prospective_memories WHERE action='expired'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "expired");
    /* existing rows get the defaults for the other columns */
    q_text(db, "SELECT cue_kind || '/' || source FROM prospective_memories WHERE action='open'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "keyword/extractor");
    HU_ASSERT_EQ(q_int(db, "SELECT attempts FROM prospective_memories WHERE action='open'"),
                 (int64_t)0);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE due_at IS NOT NULL"),
                 (int64_t)0);
    sqlite3_close(db);
}

/* The curator (scripts/insight_stream.py) writes only the legacy columns:
 * its inserts must land as pending keyword rows, and its fired updates must
 * move status with them. */
static void legacy_writers_keep_status_in_step(void) {
    sqlite3 *db = legacy_db();
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);
    legacy_insert(db, "curator row", 0);
    char s[32];
    q_text(db, "SELECT status || '/' || cue_kind FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending/keyword");

    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=2", NULL, NULL, NULL),
                 SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "canceled");
    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=3", NULL, NULL, NULL),
                 SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "expired");
    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=1", NULL, NULL, NULL),
                 SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "done");
    /* a v2 write of status alone (fired unchanged) is not overridden */
    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=0, status='pending'", NULL,
                              NULL, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(
        sqlite3_exec(db, "UPDATE prospective_memories SET status='surfaced'", NULL, NULL, NULL),
        SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "surfaced");
    sqlite3_close(db);
}

static void ensure_schema_is_idempotent(void) {
    sqlite3 *db = legacy_db();
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);
    HU_ASSERT_EQ(column_count(db), (int64_t)15);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND "
                           "name='trg_prospective_fired_status'"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                           "name='idx_prospective_status'"),
                 (int64_t)1);
    sqlite3_close(db);
}

static void ensure_schema_refuses_a_missing_table_and_null(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(NULL), HU_ERR_INVALID_ARGUMENT);
    sqlite3_close(db);
}

/* The daemon never calls ensure_schema itself: the engine does at open. */
static void engine_open_migrates_prospective_memories(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(column_count(db), (int64_t)15);
    mem.vtable->deinit(mem.ctx);
}

static void seed_kw(sqlite3 *db, const char *cue, const char *action, const char *contact,
                    int64_t created) {
    /* Bound params, not string interpolation: an action containing a quote
     * (e.g. "someone else's") would otherwise break the literal SQL. */
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "INSERT INTO prospective_memories(trigger_type,trigger_value,"
                                    "action,contact_id,expires_at,fired,created_at) "
                                    "VALUES('keyword',?1,?2,?3,0,0,?4)",
                                    -1, &st, NULL),
                 SQLITE_OK);
    sqlite3_bind_text(st, 1, cue, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, action, -1, SQLITE_STATIC);
    if (contact)
        sqlite3_bind_text(st, 3, contact, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 3);
    sqlite3_bind_int64(st, 4, created);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
}

static void repo_list_scope_order_and_exact_allocation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "flight", "ask about the flight", "+15550000001", 100);
    seed_kw(db, "dyson", "ask about the dyson", "+15550000001", 200);
    seed_kw(db, "global", "a contact-less intention", NULL, 150);
    seed_kw(db, "other", "someone else's", "+15550000002", 300);
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "call about the lease", 20,
                                                 5000, 100, HU_PM_SOURCE_PROMISE_KEEPER,
                                                 "commitment:1", HU_PM_PENDING, 10, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "return the drill", 16,
                                                 3000, 100, HU_PM_SOURCE_FOLLOWUP, "followup:7",
                                                 HU_PM_PENDING, 10, NULL),
                 HU_OK);

    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_KEYWORD, HU_PM_PENDING,
                                          "+15550000001", 12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)3); /* own two + the contact-less one; never the other contact */
    HU_ASSERT_STR_EQ(items[0].action, "ask about the dyson"); /* newest first */
    HU_ASSERT_STR_EQ(items[1].action, "a contact-less intention");
    HU_ASSERT_STR_EQ(items[2].action, "ask about the flight");
    HU_ASSERT_EQ(items[0].cue_kind, HU_PM_CUE_KEYWORD);
    HU_ASSERT_EQ(items[0].status, HU_PM_PENDING);
    HU_ASSERT_STR_EQ(items[0].trigger_value, "dyson");
    hu_prospective_repo_free(&alloc, items, n);

    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_TIME, HU_PM_PENDING, "+15550000001",
                                          12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)2);
    HU_ASSERT_STR_EQ(items[0].action, "return the drill"); /* oldest due first */
    HU_ASSERT_EQ(items[0].due_at, (int64_t)3000);
    HU_ASSERT_EQ(items[0].expires_at, (int64_t)3100);
    HU_ASSERT_STR_EQ(items[0].trigger_value, "followup:7");
    hu_prospective_repo_free(&alloc, items, n);

    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_TIME, HU_PM_SURFACED,
                                          "+15550000001", 12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)0);
    HU_ASSERT_NULL(items);
    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, (hu_prospective_cue_kind_t)9, HU_PM_PENDING,
                                          "+15550000001", 12, &items, &n),
                 HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

static void repo_transition_moves_the_whole_intention_and_fired(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "tacos", "ask about the taco place", "+15550000001", 100);
    seed_kw(db, "taco place", "ask about the taco place", "+15550000001", 100);
    seed_kw(db, "tacos", "ask about the taco place", "+15550000002", 100); /* other contact */
    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_KEYWORD, HU_PM_PENDING,
                                          "+15550000001", 12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)2);
    int changed = 0;
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &items[0], HU_PM_SURFACED, HU_PM_OUTCOME_NONE,
                                                0, 777, &changed),
                 HU_OK);
    HU_ASSERT_EQ(changed, 2); /* both keyword rows of the intention */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced' "
                           "AND surfaced_at=777 AND fired=0"),
                 (int64_t)2);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE "
                           "contact_id='+15550000002' AND status='pending'"),
                 (int64_t)1);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &items[0], HU_PM_DONE, HU_PM_OUTCOME_USED, 0,
                                                888, &changed),
                 HU_OK);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='done' AND "
                           "fired=1 AND outcome='used' AND surfaced_at=777"),
                 (int64_t)2);
    /* a settled intention is never moved again */
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &items[0], HU_PM_PENDING, HU_PM_OUTCOME_IGNORED,
                                                1, 999, &changed),
                 HU_OK);
    HU_ASSERT_EQ(changed, 0);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &items[0], (hu_prospective_status_t)9,
                                                HU_PM_OUTCOME_NONE, 0, 1, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    hu_prospective_repo_free(&alloc, items, n);
    mem.vtable->deinit(mem.ctx);
}

static void repo_count_surfaced_since_counts_intentions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "call the vet", 12, 500,
                                                 100, HU_PM_SOURCE_FOLLOWUP, "followup:1",
                                                 HU_PM_PENDING, 10, NULL),
                 HU_OK);
    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_TIME, HU_PM_PENDING, "+15550000001",
                                          12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &items[0], HU_PM_SURFACED, HU_PM_OUTCOME_NONE,
                                                0, 1000, NULL),
                 HU_OK);
    hu_prospective_repo_free(&alloc, items, n);
    int64_t c = -1;
    HU_ASSERT_EQ(
        hu_prospective_repo_count_surfaced_since(db, HU_PM_CUE_TIME, "+15550000001", 12, 900, &c),
        HU_OK);
    HU_ASSERT_EQ(c, (int64_t)1);
    HU_ASSERT_EQ(
        hu_prospective_repo_count_surfaced_since(db, HU_PM_CUE_TIME, "+15550000001", 12, 1001, &c),
        HU_OK);
    HU_ASSERT_EQ(c, (int64_t)0);
    HU_ASSERT_EQ(
        hu_prospective_repo_count_surfaced_since(db, HU_PM_CUE_TIME, "+15550000002", 12, 0, &c),
        HU_OK);
    HU_ASSERT_EQ(c, (int64_t)0);
    mem.vtable->deinit(mem.ctx);
}

/* A commitment and its paired delayed follow-up are ONE intention. */
static void repo_upsert_time_is_idempotent_by_key_and_by_intention(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    bool ins = false;
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "call about the lease", 20,
                                                 5000, 100, HU_PM_SOURCE_PROMISE_KEEPER,
                                                 "commitment:1", HU_PM_PENDING, 10, &ins),
                 HU_OK);
    HU_ASSERT_TRUE(ins);
    /* same key again */
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "call about the lease", 20,
                                                 5000, 100, HU_PM_SOURCE_PROMISE_KEEPER,
                                                 "commitment:1", HU_PM_PENDING, 10, &ins),
                 HU_OK);
    HU_ASSERT_FALSE(ins);
    /* the paired follow-up: other key, same contact + action + due */
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "call about the lease", 20,
                                                 5000, 100, HU_PM_SOURCE_FOLLOWUP, "followup:9",
                                                 HU_PM_PENDING, 10, &ins),
                 HU_OK);
    HU_ASSERT_FALSE(ins);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)1);
    char s[96];
    q_text(db,
           "SELECT trigger_type || '/' || source || '/' || status || '/' || fired || '/' || "
           "expires_at FROM prospective_memories WHERE cue_kind='time'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "time/promise_keeper/pending/0/5100");
    /* an import already past its window lands expired, fired=3 */
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "old promise", 11, 40, 100,
                                                 HU_PM_SOURCE_PROMISE_KEEPER, "commitment:2",
                                                 HU_PM_EXPIRED, 10, &ins),
                 HU_OK);
    HU_ASSERT_TRUE(ins);
    HU_ASSERT_EQ(q_int(db, "SELECT fired FROM prospective_memories WHERE action='old promise'"),
                 (int64_t)3);
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "x", 1, 0, 100,
                                                 HU_PM_SOURCE_FOLLOWUP, "followup:1", HU_PM_PENDING,
                                                 10, &ins),
                 HU_ERR_INVALID_ARGUMENT); /* no due time */
    mem.vtable->deinit(mem.ctx);
}

/* F1 (controller ruling): the OPEN-row dedupe keys on contact + normalized
 * action, IGNORING due_at — a later backfill re-run that re-anchors due_at
 * (and uses a fresh source key, e.g. a re-scanned batch id) must not create
 * a second intention for the same still-open promise. */
static void repo_upsert_time_dedupes_open_row_ignoring_due_at_reanchor(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    bool ins = false;
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "renew the passport", 18,
                                                 1000, 100, HU_PM_SOURCE_PROMISE_KEEPER,
                                                 "commitment:40", HU_PM_PENDING, 5, &ins),
                 HU_OK);
    HU_ASSERT_TRUE(ins);
    /* backfill re-run: later now, re-anchored (later) due_at, a fresh source key */
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "renew the passport", 18,
                                                 9000, 100, HU_PM_SOURCE_PROMISE_KEEPER,
                                                 "commitment:41", HU_PM_PENDING, 500, &ins),
                 HU_OK);
    HU_ASSERT_FALSE(ins);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)1);
    /* the original row's due_at is untouched by the no-op upsert */
    HU_ASSERT_EQ(q_int(db, "SELECT due_at FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)1000);

    /* once the intention is SETTLED, it no longer blocks a fresh one for the
     * same contact + action — the dedupe is for OPEN rows only. */
    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_TIME, HU_PM_PENDING, "+15550000001",
                                          12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)1);
    HU_ASSERT_EQ(
        hu_prospective_repo_transition(db, &items[0], HU_PM_DONE, HU_PM_OUTCOME_USED, 0, 600, NULL),
        HU_OK);
    hu_prospective_repo_free(&alloc, items, n);
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "renew the passport", 18,
                                                 20000, 100, HU_PM_SOURCE_PROMISE_KEEPER,
                                                 "commitment:42", HU_PM_PENDING, 700, &ins),
                 HU_OK);
    HU_ASSERT_TRUE(ins);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)2);
    mem.vtable->deinit(mem.ctx);
}

/* Seeds a raw cue_kind='time' row with whatever noisy `action` text the
 * caller wants (bound, never interpolated), so a whitespace-run or casing
 * variant can be planted verbatim without SQL-escaping it by hand. */
static void seed_time_row_raw(sqlite3 *db, const char *key, const char *action, const char *contact,
                              int64_t due_at, int64_t created) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "INSERT INTO prospective_memories(trigger_type,trigger_value,"
                                    "action,contact_id,expires_at,fired,created_at,cue_kind,due_at,"
                                    "status,source) VALUES('time',?1,?2,?3,?4,0,?5,'time',?4,"
                                    "'pending','promise_keeper')",
                                    -1, &st, NULL),
                 SQLITE_OK);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, contact, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, due_at);
    sqlite3_bind_int64(st, 5, created);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
}

/* Fix round 1, I1 (controller ruling): the F1 open-row dedupe used to
 * compare a SQL-side normalization (LOWER/TRIM + two REPLACE('  ',' ')
 * passes) against the C-side `pm_normalize_action`. SQLite's REPLACE does
 * one left-to-right, NON-overlapping scan, so each pass roughly halves a
 * whitespace run rather than collapsing it outright — a stored action with
 * 5+ consecutive whitespace characters left a residual double space that
 * never matched the single-spaced C-normalized parameter, so NOT EXISTS
 * was always true and a duplicate open intention got inserted. That is
 * exactly the bug F1 exists to prevent. The fix moves ALL normalization
 * into C (`pm_normalize_action`, applied to both the input and every
 * fetched candidate), so it must dedupe correctly regardless of run
 * length, whitespace kind, or case. */
static void repo_upsert_time_dedupes_open_row_despite_whitespace_and_case(void) {
    struct {
        const char *stored_action;   /* seeded raw: noisy whitespace/case */
        const char *incoming_action; /* what the caller passes, clean */
    } cases[] = {
        {"renew"
         "     " /* 5 spaces */
         "the passport",
         "renew the passport"},
        {"renew"
         "         " /* 9 spaces */
         "the passport",
         "renew the passport"},
        {"renew"
         " \t\n \t " /* mixed space/tab/newline */
         "the passport",
         "renew the passport"},
        {"RENEW THE PASSPORT", "renew the passport"}, /* case only */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hu_allocator_t alloc = hu_system_allocator();
        hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
        sqlite3 *db = hu_sqlite_memory_get_db(&mem);
        seed_time_row_raw(db, "commitment:seed", cases[i].stored_action, "+15550000001", 1000, 5);
        bool ins = true;
        HU_ASSERT_EQ(hu_prospective_repo_upsert_time(
                         db, "+15550000001", strlen("+15550000001"), cases[i].incoming_action,
                         strlen(cases[i].incoming_action), 9000, 100, HU_PM_SOURCE_PROMISE_KEEPER,
                         "commitment:reanchor", HU_PM_PENDING, 500, &ins),
                     HU_OK);
        HU_ASSERT_FALSE(ins);
        HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                     (int64_t)1);
        mem.vtable->deinit(mem.ctx);
    }
}

static void repo_sync_source_retires_ledger_twins(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(
        sqlite3_exec(db,
                     "INSERT INTO commitments(contact_id,description,who,deadline,status,"
                     "created_at) VALUES('+15550000001','call about the lease','me',5000,"
                     "'pending',1),('+15550000002','call about the lease','me',5000,'pending',1);"
                     "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) VALUES"
                     "('+15550000001','call about the lease',5000,0)",
                     NULL, NULL, NULL),
        SQLITE_OK);
    hu_prospective_item_t it;
    memset(&it, 0, sizeof(it));
    it.cue_kind = HU_PM_CUE_TIME;
    snprintf(it.action, sizeof(it.action), "call about the lease");
    snprintf(it.contact_id, sizeof(it.contact_id), "+15550000001");
    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_DONE, 6000), HU_OK);
    char s[64];
    q_text(db,
           "SELECT status || '/' || followed_up_at FROM commitments WHERE "
           "contact_id='+15550000001'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "followed_up/6000");
    q_text(db, "SELECT status FROM commitments WHERE contact_id='+15550000002'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending");
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups"), (int64_t)1);
    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_PENDING, 6000),
                 HU_ERR_INVALID_ARGUMENT); /* only settled states retire the ledger */
    mem.vtable->deinit(mem.ctx);
}

/* Minor (fix round 1): both WHERE clauses in sync_source require the
 * ledger row's own "still open" state (commitments.status='pending' /
 * delayed_followups.sent=0), so once the first call retires a row a
 * second call for the same intention matches nothing and changes nothing —
 * it must not re-stamp followed_up_at with the new `now`. */
static void repo_sync_source_second_call_is_a_no_op(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(
        sqlite3_exec(db,
                     "INSERT INTO commitments(contact_id,description,who,deadline,status,"
                     "created_at) VALUES('+15550000001','call about the lease','me',5000,"
                     "'pending',1);"
                     "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) VALUES"
                     "('+15550000001','call about the lease',5000,0)",
                     NULL, NULL, NULL),
        SQLITE_OK);
    hu_prospective_item_t it;
    memset(&it, 0, sizeof(it));
    it.cue_kind = HU_PM_CUE_TIME;
    snprintf(it.action, sizeof(it.action), "call about the lease");
    snprintf(it.contact_id, sizeof(it.contact_id), "+15550000001");
    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_DONE, 6000), HU_OK);
    char s[64];
    q_text(db, "SELECT status || '/' || followed_up_at FROM commitments", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "followed_up/6000");
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups"), (int64_t)1);
    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_DONE, 9999), HU_OK);
    q_text(db, "SELECT status || '/' || followed_up_at FROM commitments", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "followed_up/6000"); /* unchanged — not re-stamped to 9999 */
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups"), (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 2, I1: a who="them" F20 pair mirrors the CONTACT's raw
 * description ("text you when I land") as a REPHRASED action ("ask if
 * they still need to text you when they land") -- so it->action can never
 * text-match either ledger row's own raw column. sync_source must retire
 * both twins by the trigger_value key ("commitment:<id>") instead. */
static void repo_sync_source_retires_by_commitment_id_for_them_pair(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    const char *desc = "text you when I land";
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000009", 12, desc,
                                                strlen(desc), "them", 4, 9000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000009", 12, desc,
                                                         strlen(desc), 9000, "them", 4),
                 HU_OK);
    /* pre: both ledger rows are still open */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM commitments WHERE status='pending'"), (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM delayed_followups WHERE sent=0"), (int64_t)1);
    /* the F20 pair collapsed to ONE mirrored row (Task 3 dedupe + fix round 1) */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)1);

    hu_prospective_item_t it;
    memset(&it, 0, sizeof(it));
    it.cue_kind = HU_PM_CUE_TIME;
    it.id = q_int(db, "SELECT id FROM prospective_memories WHERE cue_kind='time'");
    q_text(db, "SELECT trigger_value FROM prospective_memories WHERE cue_kind='time'",
           it.trigger_value, sizeof(it.trigger_value));
    q_text(db, "SELECT action FROM prospective_memories WHERE cue_kind='time'", it.action,
           sizeof(it.action));
    snprintf(it.contact_id, sizeof(it.contact_id), "+15550000009");
    HU_ASSERT_STR_EQ(it.trigger_value, "commitment:1");
    /* the mirrored action is the rephrasing, never the ledger's raw text */
    HU_ASSERT_STR_EQ(it.action, "ask if they still need to text you when they land");

    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_DONE, 9500), HU_OK);

    /* post: BOTH twins retired, even though it.action never equals either
     * ledger row's raw ("text you when I land") description/topic */
    char s[64];
    q_text(db, "SELECT status FROM commitments WHERE id=1", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "followed_up");
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups"), (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 2, I1, test (2): a lone "followup:M" row (no paired
 * commitment -- e.g. daemon_dated_followup.c's situation-frame path, who
 * NULL) settles by retiring that delayed_followups row by id. */
static void repo_sync_source_retires_lone_followup_by_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    const char *topic = "they mentioned surgery tomorrow";
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000010", 12, topic,
                                                         strlen(topic), 8000, NULL, 0),
                 HU_OK);
    /* pre: open */
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups"), (int64_t)0);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM commitments"), (int64_t)0);

    hu_prospective_item_t it;
    memset(&it, 0, sizeof(it));
    it.cue_kind = HU_PM_CUE_TIME;
    it.id = q_int(db, "SELECT id FROM prospective_memories WHERE cue_kind='time'");
    q_text(db, "SELECT trigger_value FROM prospective_memories WHERE cue_kind='time'",
           it.trigger_value, sizeof(it.trigger_value));
    snprintf(it.action, sizeof(it.action), "%s", topic);
    snprintf(it.contact_id, sizeof(it.contact_id), "+15550000010");
    HU_ASSERT_STR_EQ(it.trigger_value, "followup:1");

    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_CANCELED, 8500), HU_OK);

    /* post: retired by id; no commitments row ever existed to touch */
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups"), (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM commitments"), (int64_t)0);
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 2, I1, test (3): the owner/"me" path -- where the mirrored
 * action IS the raw verbatim text -- still retires correctly through the
 * NEW id-keyed path (not just through the old text-match fallback the
 * two tests above already cover). */
static void repo_sync_source_retires_by_commitment_id_for_owner_pair(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    const char *desc = "call the dentist";
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000011", 12, desc,
                                                strlen(desc), "me", 2, 9000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000011", 12, desc,
                                                         strlen(desc), 9000, "me", 2),
                 HU_OK);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM commitments WHERE status='pending'"), (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM delayed_followups WHERE sent=0"), (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)1);

    hu_prospective_item_t it;
    memset(&it, 0, sizeof(it));
    it.cue_kind = HU_PM_CUE_TIME;
    it.id = q_int(db, "SELECT id FROM prospective_memories WHERE cue_kind='time'");
    q_text(db, "SELECT trigger_value FROM prospective_memories WHERE cue_kind='time'",
           it.trigger_value, sizeof(it.trigger_value));
    q_text(db, "SELECT action FROM prospective_memories WHERE cue_kind='time'", it.action,
           sizeof(it.action));
    snprintf(it.contact_id, sizeof(it.contact_id), "+15550000011");
    HU_ASSERT_STR_EQ(it.trigger_value, "commitment:1");
    HU_ASSERT_STR_EQ(it.action, "call the dentist"); /* owner path: verbatim, unchanged */

    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_DONE, 9500), HU_OK);

    char s[64];
    q_text(db, "SELECT status FROM commitments WHERE id=1", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "followed_up");
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups"), (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 3 (defense in depth): trigger_value's rowid is only ever
 * minted by hu_prospective_repo_mirror_time for the row it keys, so this never happens
 * through the real writers -- but nothing in this function's contract
 * verifies that, and a hand-built or corrupted key would otherwise let
 * contact A's intention retire contact B's commitment. The scoped lookup
 * must find nothing for A and leave B's row untouched. */
static void repo_sync_source_commitment_id_is_contact_scoped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "INSERT INTO commitments(contact_id,description,who,deadline,status,"
                              "created_at) VALUES('+15550000020','A''s own promise','me',5000,"
                              "'pending',1),('+15550000021','B''s promise','me',5000,'pending',1)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    char s[64];
    /* pre: B's commitment (id=2) is pending */
    q_text(db, "SELECT status FROM commitments WHERE contact_id='+15550000021'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending");

    hu_prospective_item_t it;
    memset(&it, 0, sizeof(it));
    it.cue_kind = HU_PM_CUE_TIME;
    snprintf(it.contact_id, sizeof(it.contact_id), "+15550000020");       /* contact A */
    snprintf(it.trigger_value, sizeof(it.trigger_value), "commitment:2"); /* B's row */
    snprintf(it.action, sizeof(it.action), "whatever");

    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_DONE, 9000), HU_OK);

    /* post: B's commitment is UNTOUCHED -- the contact-scoped lookup found
     * nothing for contact A under id=2 (it belongs to B), and the
     * dispatcher never falls back to the text-match path once the key
     * parses */
    q_text(db, "SELECT status FROM commitments WHERE contact_id='+15550000021'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending");
    /* A's own row (id=1) was never named by the key either */
    q_text(db, "SELECT status FROM commitments WHERE contact_id='+15550000020'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending");
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 3, same hazard on the followup:M path. */
static void repo_sync_source_followup_id_is_contact_scoped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) "
                              "VALUES('+15550000022','A follow-up',5000,0),"
                              "('+15550000023','B follow-up',5000,0)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    char s[8];
    q_text(db, "SELECT sent FROM delayed_followups WHERE contact_id='+15550000023'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "0");

    hu_prospective_item_t it;
    memset(&it, 0, sizeof(it));
    it.cue_kind = HU_PM_CUE_TIME;
    snprintf(it.contact_id, sizeof(it.contact_id), "+15550000022");     /* contact A */
    snprintf(it.trigger_value, sizeof(it.trigger_value), "followup:2"); /* B's row */
    snprintf(it.action, sizeof(it.action), "whatever");

    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_DONE, 9000), HU_OK);

    q_text(db, "SELECT sent FROM delayed_followups WHERE contact_id='+15550000023'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "0"); /* B's row untouched */
    q_text(db, "SELECT sent FROM delayed_followups WHERE contact_id='+15550000022'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "0"); /* A's own row (id=1) was never named either */
    mem.vtable->deinit(mem.ctx);
}

/* Known gap 2: the legacy F31 send marks a delayed follow-up sent through
 * hu_superhuman_delayed_followup_mark_sent. Its v2 twin -- here the F20
 * pair's collapsed row, keyed "commitment:1", which the follow-up mirrored
 * into -- must end DONE/USED, or the time path surfaces the topic again.
 * Another contact's identical topic and the same contact's other follow-up
 * stay open; the ledger is left exactly as the legacy path wrote it. */
static void repo_legacy_mark_sent_settles_the_v2_twin(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    const char *desc = "call the dentist";
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000030", 12, desc,
                                                strlen(desc), "me", 2, 9000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000030", 12, desc,
                                                         strlen(desc), 9000, "me", 2),
                 HU_OK); /* followup id 1, collapsed into commitment:1 */
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000031", 12, desc,
                                                         strlen(desc), 9000, NULL, 0),
                 HU_OK); /* followup id 2: another contact, same words */
    const char *other = "return the drill";
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000030", 12, other,
                                                         strlen(other), 9000, NULL, 0),
                 HU_OK); /* followup id 3: same contact, another topic */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time' AND "
                           "status='pending'"),
                 (int64_t)3);
    char s[128];
    q_text(db,
           "SELECT trigger_value FROM prospective_memories WHERE contact_id='+15550000030' "
           "AND action='call the dentist'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "commitment:1");

    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 1), HU_OK);

    q_text(db,
           "SELECT status || '/' || fired || '/' || outcome FROM prospective_memories WHERE "
           "trigger_value='commitment:1'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "done/1/used");
    q_text(db, "SELECT status FROM prospective_memories WHERE contact_id='+15550000031'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending"); /* the other contact's identical topic */
    q_text(db, "SELECT status FROM prospective_memories WHERE action='return the drill'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending"); /* the same contact's other follow-up */
    /* the ledger: only the row the legacy path marked moved */
    HU_ASSERT_EQ(q_int(db, "SELECT group_concat(id || ':' || sent) = '1:1,2:0,3:0' FROM "
                           "(SELECT id, sent FROM delayed_followups ORDER BY id)"),
                 (int64_t)1);
    q_text(db, "SELECT status FROM commitments WHERE id=1", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending"); /* no ledger cascade: the legacy path owns it */

    /* a lone keyed twin ("followup:3") settles by its key */
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 3), HU_OK);
    q_text(db,
           "SELECT status || '/' || outcome FROM prospective_memories WHERE "
           "trigger_value='followup:3'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "done/used");
    q_text(db, "SELECT status FROM prospective_memories WHERE contact_id='+15550000031'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending");
    mem.vtable->deinit(mem.ctx);
}

/* No twin: a contact's promise the mirror refused (F4, "to ") has no time
 * row, an already-sent row changes nothing, and a missing id is fine --
 * mark_sent stays HU_OK and writes no prospective row. */
static void repo_legacy_mark_sent_without_a_twin_is_a_no_op(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000032", 12, "to ", 3,
                                                         9000, "them", 4),
                 HU_OK);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories"), (int64_t)0);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 1), HU_OK);
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups WHERE id=1"), (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories"), (int64_t)0);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 99), HU_OK);
    int changed = -1;
    HU_ASSERT_EQ(hu_prospective_repo_settle_followup_twin(db, 99, 9000, &changed), HU_OK);
    HU_ASSERT_EQ(changed, 0);
    HU_ASSERT_EQ(hu_prospective_repo_settle_followup_twin(NULL, 1, 9000, &changed),
                 HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

/* Known gap 4, commitment side of the same shape: two owner commitments
 * with the same words (different deadlines) collapse into ONE open row;
 * settling it retires both, while the same contact's other promise and
 * another contact's identical one stay pending. */
static void repo_sync_source_retires_every_collapsed_ledger_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    static const char *const d[] = {"call the dentist", "call  The Dentist", "call the dentist",
                                    "return the drill"};
    static const char *const c[] = {"+15550000033", "+15550000033", "+15550000034", "+15550000033"};
    for (int i = 0; i < 4; i++)
        HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, c[i], 12, d[i], strlen(d[i]),
                                                    "me", 2, 9000 + i * 86400),
                     HU_OK);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE contact_id="
                           "'+15550000033'"),
                 (int64_t)2); /* dentist x2 collapsed + drill */
    hu_prospective_item_t it;
    memset(&it, 0, sizeof(it));
    it.cue_kind = HU_PM_CUE_TIME;
    q_text(db,
           "SELECT trigger_value FROM prospective_memories WHERE contact_id='+15550000033' AND "
           "action='call the dentist'",
           it.trigger_value, sizeof(it.trigger_value));
    HU_ASSERT_STR_EQ(it.trigger_value, "commitment:1");
    snprintf(it.action, sizeof(it.action), "call the dentist");
    snprintf(it.contact_id, sizeof(it.contact_id), "+15550000033");
    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_CANCELED, 9500), HU_OK);
    HU_ASSERT_EQ(q_int(db, "SELECT group_concat(id || ':' || status) = "
                           "'1:canceled,2:canceled,3:pending,4:pending' FROM "
                           "(SELECT id, status FROM commitments ORDER BY id)"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

void run_prospective_repo_sqlite_tests(void) {
    HU_TEST_SUITE("prospective repo");
    HU_RUN_TEST(ensure_schema_adds_typed_columns_and_maps_fired);
    HU_RUN_TEST(legacy_writers_keep_status_in_step);
    HU_RUN_TEST(ensure_schema_is_idempotent);
    HU_RUN_TEST(ensure_schema_refuses_a_missing_table_and_null);
    HU_RUN_TEST(engine_open_migrates_prospective_memories);
    HU_RUN_TEST(repo_list_scope_order_and_exact_allocation);
    HU_RUN_TEST(repo_transition_moves_the_whole_intention_and_fired);
    HU_RUN_TEST(repo_count_surfaced_since_counts_intentions);
    HU_RUN_TEST(repo_upsert_time_is_idempotent_by_key_and_by_intention);
    HU_RUN_TEST(repo_upsert_time_dedupes_open_row_ignoring_due_at_reanchor);
    HU_RUN_TEST(repo_upsert_time_dedupes_open_row_despite_whitespace_and_case);
    HU_RUN_TEST(repo_sync_source_retires_ledger_twins);
    HU_RUN_TEST(repo_sync_source_second_call_is_a_no_op);
    HU_RUN_TEST(repo_sync_source_retires_by_commitment_id_for_them_pair);
    HU_RUN_TEST(repo_sync_source_retires_lone_followup_by_id);
    HU_RUN_TEST(repo_sync_source_retires_by_commitment_id_for_owner_pair);
    HU_RUN_TEST(repo_sync_source_commitment_id_is_contact_scoped);
    HU_RUN_TEST(repo_sync_source_followup_id_is_contact_scoped);
    HU_RUN_TEST(repo_legacy_mark_sent_settles_the_v2_twin);
    HU_RUN_TEST(repo_legacy_mark_sent_without_a_twin_is_a_no_op);
    HU_RUN_TEST(repo_sync_source_retires_every_collapsed_ledger_row);
}

#else

void run_prospective_repo_sqlite_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
