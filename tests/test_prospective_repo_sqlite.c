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
    HU_RUN_TEST(repo_sync_source_retires_ledger_twins);
}

#else

void run_prospective_repo_sqlite_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
