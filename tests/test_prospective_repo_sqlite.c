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
#include "human/memory/prospective_v2.h"
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
           "SELECT status || '/' || fired || '/' || ifnull(outcome, 'none') FROM "
           "prospective_memories WHERE trigger_value='commitment:1'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "done/1/none"); /* no evidence claim: the legacy path's send */
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
           "SELECT status || '/' || ifnull(outcome, 'none') FROM prospective_memories WHERE "
           "trigger_value='followup:3'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "done/none");
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

#define PM_T0 ((int64_t)1790000000)
#define PM_D  ((int64_t)86400)

/* The open time row of `contact` with `action`, as the v2 pass lists it. */
static void open_item(hu_allocator_t *alloc, sqlite3 *db, const char *contact, const char *action,
                      hu_prospective_item_t *out) {
    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_prospective_repo_list(alloc, db, HU_PM_CUE_TIME, HU_PM_PENDING, contact,
                                          strlen(contact), &items, &n),
                 HU_OK);
    bool found = false;
    for (size_t i = 0; i < n && !found; i++)
        if (strcmp(items[i].action, action) == 0) {
            *out = items[i];
            found = true;
        }
    hu_prospective_repo_free(alloc, items, n);
    HU_ASSERT_TRUE(found);
}

/* pm_retire's order: the intention goes terminal, then its ledger follows. */
static void settle(sqlite3 *db, const hu_prospective_item_t *it, hu_prospective_status_t to,
                   int64_t now) {
    int ch = 0;
    HU_ASSERT_EQ(
        hu_prospective_repo_transition(db, it, to, HU_PM_OUTCOME_NONE, it->attempts, now, &ch),
        HU_OK);
    HU_ASSERT_TRUE(ch > 0);
    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, it, to, now), HU_OK);
}

/* Fix round 1, CRITICAL A: the gap-4 sweep retires only the collapsed rows
 * due within the intention's grace window (due <= due_at + grace). A
 * same-words sibling dated beyond it is a later promise F1 merely folded in
 * while the first was open: it survives, and gets its OWN fresh open time
 * row keyed by its own ledger id with its own due -- for DONE, CANCELED and
 * EXPIRED alike. The same contact's other promise and another contact's
 * identical one are untouched. */
static void repo_sweep_is_bounded_by_grace_and_remirrors_the_survivor(void) {
    static const struct {
        hu_prospective_status_t to;
        const char *ledger;
    } k[] = {{HU_PM_DONE, "followed_up"}, {HU_PM_CANCELED, "canceled"}, {HU_PM_EXPIRED, "expired"}};
    for (size_t t = 0; t < sizeof(k) / sizeof(k[0]); t++) {
        hu_allocator_t alloc = hu_system_allocator();
        hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
        sqlite3 *db = hu_sqlite_memory_get_db(&mem);
        static const struct {
            const char *contact;
            const char *desc;
            int64_t due;
        } c[] = {{"+15550000033", "call the dentist", PM_T0},             /* #1: keys the row */
                 {"+15550000033", "call  The Dentist", PM_T0 + PM_D},     /* #2: within grace */
                 {"+15550000033", "call the dentist", PM_T0 + 11 * PM_D}, /* #3: beyond */
                 {"+15550000033", "return the drill", PM_T0},
                 {"+15550000034", "call the dentist", PM_T0}};
        for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++)
            HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, c[i].contact, 12, c[i].desc,
                                                        strlen(c[i].desc), "me", 2, c[i].due),
                         HU_OK);
        HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE contact_id="
                               "'+15550000033'"),
                     (int64_t)2); /* dentist x3 collapsed into commitment:1, + drill */
        hu_prospective_item_t it;
        open_item(&alloc, db, "+15550000033", "call the dentist", &it);
        HU_ASSERT_STR_EQ(it.trigger_value, "commitment:1");
        settle(db, &it, k[t].to, PM_T0 + 3600);

        char want[160];
        snprintf(want, sizeof(want), "1:%s,2:%s,3:pending,4:pending,5:pending", k[t].ledger,
                 k[t].ledger);
        char got[160];
        q_text(db,
               "SELECT group_concat(id || ':' || status) FROM "
               "(SELECT id, status FROM commitments ORDER BY id)",
               got, sizeof(got));
        HU_ASSERT_STR_EQ(got, want);
        /* the survivor is represented again: its own key, its own due */
        char row[160];
        q_text(db,
               "SELECT action || '|' || status || '|' || due_at FROM prospective_memories WHERE "
               "trigger_value='commitment:3'",
               row, sizeof(row));
        HU_ASSERT_STR_EQ(row, "call the dentist|pending|1790950400");
        HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE contact_id="
                               "'+15550000033' AND status IN ('pending','surfaced')"),
                     (int64_t)2); /* the survivor's row + drill: one per intention */
        mem.vtable->deinit(mem.ctx);
    }
}

/* Same bound on the F20 twin retire by description: two F20 pairs with the
 * same words, the second due two weeks later, collapse into commitment:1.
 * Settling it retires the first pair only; the second pair's commitment AND
 * follow-up stay open, and the second pair is re-mirrored under its own id. */
static void repo_sync_source_f20_twin_retire_is_bounded(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    const char *desc = "call mom";
    for (int i = 0; i < 2; i++) {
        int64_t due = PM_T0 + i * 14 * PM_D;
        HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000035", 12, desc,
                                                    strlen(desc), "me", 2, due),
                     HU_OK);
        HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000035", 12, desc,
                                                             strlen(desc), due, "me", 2),
                     HU_OK);
    }
    hu_prospective_item_t it;
    open_item(&alloc, db, "+15550000035", "call mom", &it);
    HU_ASSERT_STR_EQ(it.trigger_value, "commitment:1");
    settle(db, &it, HU_PM_DONE, PM_T0 + 3600);
    char got[128];
    q_text(db,
           "SELECT (SELECT group_concat(status) FROM (SELECT status FROM commitments ORDER BY id))"
           " || '/' || (SELECT group_concat(sent) FROM (SELECT sent FROM delayed_followups ORDER "
           "BY id))",
           got, sizeof(got));
    HU_ASSERT_STR_EQ(got, "followed_up,pending/1,0");
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE "
                           "trigger_value='commitment:2' AND status='pending' AND "
                           "due_at=1791209600"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 1, IMPORTANT C1: a twin v2 already SURFACED belongs to
 * after_delivery -- only it can say whether the reply used the action. The
 * legacy mark-sent leaves it surfaced; the delivery that follows judges it
 * (here: used -> done/used on evidence). */
static void repo_legacy_mark_sent_skips_a_surfaced_twin(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    const char *desc = "call the dentist";
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000036", 12, desc,
                                                strlen(desc), "me", 2, PM_T0),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000036", 12, desc,
                                                         strlen(desc), PM_T0, "me", 2),
                 HU_OK);
    hu_prospective_item_t it;
    open_item(&alloc, db, "+15550000036", desc, &it);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &it, HU_PM_SURFACED, HU_PM_OUTCOME_NONE, 0,
                                                PM_T0 + 60, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 1), HU_OK);
    char s[64];
    q_text(db, "SELECT status || '/' || ifnull(outcome, 'none') FROM prospective_memories", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "surfaced/none"); /* untouched by the legacy path */
    static const char reply[] = "did you ever call the dentist?";
    hu_prospective_delivery_counts_t dc;
    HU_ASSERT_EQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_TIME, "+15550000036", 12,
                                                  reply, sizeof(reply) - 1, PM_T0 + 120, &dc),
                 HU_OK);
    HU_ASSERT_EQ(dc.used, (size_t)1);
    q_text(db, "SELECT status || '/' || ifnull(outcome, 'none') FROM prospective_memories", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "done/used"); /* judged by after_delivery, on evidence */
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 1, IMPORTANT C2: the action-match fallback is due-bounded like
 * the sweep. A never-mirrored follow-up "call mom" due T is marked sent;
 * the open "call mom" row due T+10d is a LATER promise and stays open. A
 * follow-up due within grace of that row does close it (control). */
static void repo_legacy_mark_sent_leaves_a_later_dated_same_action_row_open(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) VALUES"
             "('+15550000037','call mom',%lld,0),('+15550000037','call mom',%lld,0)",
             (long long)PM_T0, (long long)(PM_T0 + 9 * PM_D));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK); /* pre-mirror rows */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000037", 12, "call mom", 8,
                                                "me", 2, PM_T0 + 10 * PM_D),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 1), HU_OK);
    char s[32];
    q_text(db, "SELECT status FROM prospective_memories WHERE trigger_value='commitment:1'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending"); /* due T+10d > T + grace: not this follow-up's twin */
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 2), HU_OK);
    q_text(db, "SELECT status FROM prospective_memories WHERE trigger_value='commitment:1'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "done"); /* due T+10d <= T+9d + grace */
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 2, I2 (probe P2): the legacy settle runs the same bounded
 * sweep + survivor re-mirror as a v2 settle. Two "call mom" F20 pairs 14
 * days apart collapse into commitment:1; marking the first follow-up sent
 * closes commitment:1 AND gives the second pair a fresh open row keyed by
 * its own id with its own due. The ledger is the legacy path's: only the
 * row it marked moved (OFF stays byte-identical). */
static void repo_legacy_mark_sent_remirrors_a_later_sibling(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    for (int i = 0; i < 2; i++) {
        int64_t due = PM_T0 + i * 14 * PM_D;
        HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000038", 12, "call mom", 8,
                                                    "me", 2, due),
                     HU_OK);
        HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000038", 12,
                                                             "call mom", 8, due, "me", 2),
                     HU_OK);
    }
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories"), (int64_t)1);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 1), HU_OK);
    char s[64];
    q_text(db, "SELECT status FROM prospective_memories WHERE trigger_value='commitment:1'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "done");
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE "
                           "trigger_value='commitment:2' AND status='pending' AND "
                           "due_at=1791209600"),
                 (int64_t)1);
    q_text(db,
           "SELECT (SELECT group_concat(status) FROM (SELECT status FROM commitments ORDER BY id))"
           " || '/' || (SELECT group_concat(sent) FROM (SELECT sent FROM delayed_followups ORDER "
           "BY id))",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending,pending/1,0");
    /* idempotent: the ledger row is already sent, nothing moves again */
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 1), HU_OK);
    int changed = -1;
    HU_ASSERT_EQ(hu_prospective_repo_settle_followup_twin(db, 1, PM_T0, &changed), HU_OK);
    HU_ASSERT_EQ(changed, 0);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending'"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

/* Known gap 6: three same-words F20 pairs 2.5 days apart (T0, T0+2.5d,
 * T0+5d) collapse into commitment:1. Marking the MIDDLE follow-up sent
 * closes commitment:1 (its due T0 is within the follow-up's window) and the
 * bounded sweep re-mirrors the T0+5d pair as commitment:3 -- a later
 * promise. That fresh row's due (T0+5d) is inside the middle follow-up's
 * own action window (T0+2.5d + 3d), so the twin loop's next pass used to
 * action-match it and close it as done/no-outcome while its ledger rows
 * stayed pending: v2 would never surface that promise. A row this call
 * re-mirrored is never this follow-up's twin. */
static void repo_legacy_mark_sent_keeps_the_survivor_it_remirrored(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    static const int64_t due[] = {PM_T0, PM_T0 + 5 * PM_D / 2, PM_T0 + 5 * PM_D};
    for (size_t i = 0; i < sizeof(due) / sizeof(due[0]); i++) {
        HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000042", 12, "call mom", 8,
                                                    "me", 2, due[i]),
                     HU_OK);
        HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000042", 12,
                                                             "call mom", 8, due[i], "me", 2),
                     HU_OK); /* pair i+1: commitment i+1, follow-up i+1 */
    }
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories"), (int64_t)1);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 2), HU_OK);

    char s[160];
    q_text(db, "SELECT status FROM prospective_memories WHERE trigger_value='commitment:1'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "done"); /* the twin of the middle follow-up */
    /* the T0+5d promise: an OPEN row keyed by its own ledger id, its own due */
    q_text(db,
           "SELECT status || '|' || attempts || '|' || ifnull(outcome, 'none') || '|' || due_at "
           "FROM prospective_memories WHERE trigger_value='commitment:3'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending|0|none|1790432000");
    /* ...and its ledger rows are still pending: the legacy path marked only #2 */
    q_text(db,
           "SELECT (SELECT group_concat(status) FROM (SELECT status FROM commitments ORDER BY id))"
           " || '/' || (SELECT group_concat(sent) FROM (SELECT sent FROM delayed_followups ORDER "
           "BY id))",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending,pending,pending/0,1,0");
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status IN "
                           "('pending','surfaced')"),
                 (int64_t)1); /* exactly one open intention: the later promise */

    /* that row is still the third follow-up's twin: its own send settles it */
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 3), HU_OK);
    q_text(db, "SELECT status FROM prospective_memories WHERE trigger_value='commitment:3'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "done");
    mem.vtable->deinit(mem.ctx);
}

/* Known gap 7: v2 owns a ledger row when an OPEN time row of its contact is
 * its twin -- by its own key, its F20 partner's key, or (dated) its mirror
 * text within its due + grace -- and keeps owning it while surfaced or
 * retrying. A terminal twin, a later same-words promise, another contact's
 * identical words and a missing row are not owned. */
static void repo_ledger_v2_owned_by_key_f20_and_bounded_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    bool owned = false;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000043", 12,
                                                         "renew the passport", 18, PM_T0, "me", 2),
                 HU_OK); /* follow-up 1: its own row, followup:1 */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000043", 12, "call mom", 8,
                                                "me", 2, PM_T0 + PM_D),
                 HU_OK); /* commitment 1: commitment:1 */
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000043", 12,
                                                         "call mom", 8, PM_T0 + PM_D, "me", 2),
                 HU_OK); /* follow-up 2: its F20 pair, collapsed into commitment:1 */
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) VALUES"
             "('+15550000043','call mom',%lld,0)," /* 3: same words, 11 days earlier */
             "('+15550000044','call mom',%lld,0)," /* 4: another contact */
             "('+15550000043','call mom',%lld,0)", /* 5: unkeyed, within the window */
             (long long)(PM_T0 - 10 * PM_D), (long long)(PM_T0 + PM_D), (long long)PM_T0);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK); /* pre-mirror rows */

    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, true, 1, &owned), HU_OK);
    HU_ASSERT_TRUE(owned); /* own key */
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, true, 2, &owned), HU_OK);
    HU_ASSERT_TRUE(owned); /* the F20 partner's key */
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, false, 1, &owned), HU_OK);
    HU_ASSERT_TRUE(owned); /* a commitment, by its own key */
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, true, 5, &owned), HU_OK);
    HU_ASSERT_TRUE(owned); /* due T0+1d <= T0 + grace: the bounded action match */
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, true, 3, &owned), HU_OK);
    HU_ASSERT_FALSE(owned); /* T0+1d > T0-10d + grace: a different promise */
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, true, 4, &owned), HU_OK);
    HU_ASSERT_FALSE(owned); /* another contact's identical words */

    /* surfaced, and back to pending for its retry: still v2's */
    hu_prospective_item_t it;
    open_item(&alloc, db, "+15550000043", "call mom", &it);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &it, HU_PM_SURFACED, HU_PM_OUTCOME_NONE, 0,
                                                PM_T0 + 60, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, true, 2, &owned), HU_OK);
    HU_ASSERT_TRUE(owned);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &it, HU_PM_PENDING, HU_PM_OUTCOME_IGNORED, 1,
                                                PM_T0 + 120, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, true, 2, &owned), HU_OK);
    HU_ASSERT_TRUE(owned);

    /* a terminal twin owns nothing; the ledger row is untouched by the question */
    open_item(&alloc, db, "+15550000043", "renew the passport", &it);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &it, HU_PM_EXPIRED, HU_PM_OUTCOME_NONE, 0,
                                                PM_T0 + 60, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, true, 1, &owned), HU_OK);
    HU_ASSERT_FALSE(owned);
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups WHERE id=1"), (int64_t)0);

    owned = true;
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, true, 99, &owned), HU_OK);
    HU_ASSERT_FALSE(owned); /* no such row */
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(NULL, true, 1, &owned),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, true, 1, NULL), HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

/* Review minor: a commitment whose open twin is keyed by its F20 follow-up
 * partner (followup:<id>) is v2's too. The follow-up is scheduled first, so
 * the twin carries the follow-up's key and the commitment collapses into it;
 * only pm_twin_f20's delayed_followups lookup can find it from the commitment. */
static void repo_ledger_v2_owned_commitment_by_f20_partner(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    bool owned = false;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(
                     &mem, &alloc, "+15550000045", 12, "fix the fence", 13, PM_T0 + PM_D, "me", 2),
                 HU_OK); /* follow-up 1: twin followup:1 */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000045", 12, "fix the fence",
                                                13, "me", 2, PM_T0 + PM_D),
                 HU_OK); /* commitment 1: its F20 pair */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE "
                           "trigger_value='commitment:1' AND status IN ('pending','surfaced')"),
                 (int64_t)0); /* precondition: no twin under the commitment's own key */
    HU_ASSERT_EQ(hu_prospective_repo_ledger_v2_owned(db, false, 1, &owned), HU_OK);
    HU_ASSERT_TRUE(owned);
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 2, minor P3: after_delivery judged the surfaced row IGNORED and
 * put it back to pending for its retry (attempts 1). A later legacy
 * mark-sent must not close it: v2 owns every row it has ever surfaced. */
static void repo_legacy_mark_sent_skips_a_twin_v2_ever_surfaced(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    const char *desc = "call the dentist";
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000039", 12, desc,
                                                strlen(desc), "me", 2, PM_T0),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000039", 12, desc,
                                                         strlen(desc), PM_T0, "me", 2),
                 HU_OK);
    hu_prospective_item_t it;
    open_item(&alloc, db, "+15550000039", desc, &it);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &it, HU_PM_SURFACED, HU_PM_OUTCOME_NONE, 0,
                                                PM_T0 + 60, NULL),
                 HU_OK);
    static const char reply[] = "sounds good";
    HU_ASSERT_EQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_TIME, "+15550000039", 12,
                                                  reply, sizeof(reply) - 1, PM_T0 + 120, NULL),
                 HU_OK);
    char s[64];
    q_text(db, "SELECT status || '/' || attempts FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending/1"); /* ignored: back for its retry */
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 1), HU_OK);
    q_text(db, "SELECT status || '/' || attempts FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending/1"); /* the retry survives the legacy send */
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 2, minor (undated): an undated ledger row (deadline 0) is out
 * of every bound -- never swept by text -- and a follow-up with no
 * scheduled_at gets key/F20-key matching only, never the action match. */
static void repo_undated_rows_are_out_of_bound(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(
        hu_superhuman_commitment_store(&mem, &alloc, "+15550000040", 12, "call mom", 8, "me", 2, 0),
        HU_OK); /* #1: undated, never mirrored */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000040", 12, "call mom", 8,
                                                "me", 2, PM_T0),
                 HU_OK); /* #2: keys the row */
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) "
                              "VALUES('+15550000040','call mom',0,0)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, 1), HU_OK);
    char s[64];
    q_text(db, "SELECT status FROM prospective_memories WHERE trigger_value='commitment:2'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending"); /* an undated follow-up proves nothing by its words */
    hu_prospective_item_t it;
    open_item(&alloc, db, "+15550000040", "call mom", &it);
    settle(db, &it, HU_PM_DONE, PM_T0 + 60);
    q_text(db, "SELECT group_concat(status) FROM (SELECT status FROM commitments ORDER BY id)", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending,followed_up"); /* the undated #1 is not swept */
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 2, minor (atomicity): settle + ledger sync + re-mirror are one
 * unit. A ledger write that fails (a test-only trigger) rolls the whole
 * unit back -- the intention stays open, nothing half-applied -- and the
 * error is returned, not swallowed. */
static void repo_settle_is_one_unit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000041", 12,
                                                "renew the passport", 18, "me", 2, PM_T0),
                 HU_OK);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "CREATE TRIGGER t_boom BEFORE UPDATE ON commitments BEGIN "
                              "SELECT RAISE(ABORT, 'injected'); END;",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    hu_prospective_item_t it;
    open_item(&alloc, db, "+15550000041", "renew the passport", &it);
    int changed = -1;
    HU_ASSERT_NEQ(
        hu_prospective_repo_settle(db, &it, HU_PM_DONE, HU_PM_OUTCOME_NONE, 0, PM_T0, &changed),
        HU_OK);
    HU_ASSERT_EQ(changed, 0);
    char s[64];
    q_text(db, "SELECT status || '/' || fired FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending/0"); /* rolled back with the failed sync */
    HU_ASSERT_TRUE(sqlite3_get_autocommit(db) != 0);
    /* through the v2 delivery path: the error propagates, the row stays */
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &it, HU_PM_SURFACED, HU_PM_OUTCOME_NONE, 0,
                                                PM_T0 + 60, NULL),
                 HU_OK);
    static const char reply[] = "did you renew the passport?";
    HU_ASSERT_NEQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_TIME, "+15550000041", 12,
                                                   reply, sizeof(reply) - 1, PM_T0 + 120, NULL),
                  HU_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "surfaced");
    HU_ASSERT_EQ(sqlite3_exec(db, "DROP TRIGGER t_boom", NULL, NULL, NULL), SQLITE_OK);
    HU_ASSERT_EQ(
        hu_prospective_repo_settle(db, &it, HU_PM_DONE, HU_PM_OUTCOME_NONE, 0, PM_T0, &changed),
        HU_OK);
    HU_ASSERT_EQ(changed, 1);
    q_text(db, "SELECT status FROM commitments", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "followed_up");
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
    HU_RUN_TEST(repo_sweep_is_bounded_by_grace_and_remirrors_the_survivor);
    HU_RUN_TEST(repo_sync_source_f20_twin_retire_is_bounded);
    HU_RUN_TEST(repo_legacy_mark_sent_skips_a_surfaced_twin);
    HU_RUN_TEST(repo_legacy_mark_sent_leaves_a_later_dated_same_action_row_open);
    HU_RUN_TEST(repo_legacy_mark_sent_remirrors_a_later_sibling);
    HU_RUN_TEST(repo_legacy_mark_sent_keeps_the_survivor_it_remirrored);
    HU_RUN_TEST(repo_ledger_v2_owned_by_key_f20_and_bounded_action);
    HU_RUN_TEST(repo_ledger_v2_owned_commitment_by_f20_partner);
    HU_RUN_TEST(repo_legacy_mark_sent_skips_a_twin_v2_ever_surfaced);
    HU_RUN_TEST(repo_undated_rows_are_out_of_bound);
    HU_RUN_TEST(repo_settle_is_one_unit);
}

#else

void run_prospective_repo_sqlite_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
