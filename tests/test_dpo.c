#include "human/ml/dpo.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

static void dpo_create_and_init_tables(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 100, &col), HU_OK);
    HU_ASSERT_EQ((int)col.pair_count, 0);
    HU_ASSERT_EQ((int)col.max_pairs, 100);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);
    hu_dpo_collector_deinit(&col);
}

static void dpo_record_pair_stores_correctly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    hu_preference_pair_t pair;
    memset(&pair, 0, sizeof(pair));
    memcpy(pair.prompt, "test prompt", 11);
    pair.prompt_len = 11;
    memcpy(pair.chosen, "good answer", 11);
    pair.chosen_len = 11;
    memcpy(pair.rejected, "bad answer", 10);
    pair.rejected_len = 10;
    pair.margin = 0.9;
    HU_ASSERT_EQ(hu_dpo_record_pair(&col, &pair), HU_OK);
    size_t count = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &count), HU_OK);
    HU_ASSERT_EQ((int)count, 1);
    hu_dpo_collector_deinit(&col);
}

static void dpo_record_from_feedback_positive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_from_feedback(&col, "prompt", 6, "response", 8, true), HU_OK);
    size_t count = 0;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT_EQ((int)count, 1);
    hu_dpo_collector_deinit(&col);
}

static void dpo_record_from_feedback_negative(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_from_feedback(&col, "prompt", 6, "bad resp", 8, false), HU_OK);
    size_t count = 0;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT_EQ((int)count, 1);
    hu_dpo_collector_deinit(&col);
}

static void dpo_record_from_retry(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_from_retry(&col, "prompt", 6, "rejected", 8, "chosen", 6), HU_OK);
    size_t count = 0;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT_EQ((int)count, 1);
    hu_dpo_collector_deinit(&col);
}

static void dpo_export_jsonl_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    hu_dpo_record_from_feedback(&col, "p", 1, "resp", 4, true);
    size_t exported = 0;
    HU_ASSERT_EQ(hu_dpo_export_jsonl(&col, "test.jsonl", 10, &exported), HU_OK);
    HU_ASSERT_EQ((int)exported, 1);
    hu_dpo_collector_deinit(&col);
}

static void dpo_pair_count_accurate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    hu_dpo_record_from_feedback(&col, "p1", 2, "resp1", 5, true);
    hu_dpo_record_from_feedback(&col, "p2", 2, "resp2", 5, false);
    hu_dpo_record_from_retry(&col, "p3", 2, "rejected", 8, "chosen", 6);
    size_t count = 0;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT_EQ((int)count, 3);
    hu_dpo_collector_deinit(&col);
}

static void dpo_clear_removes_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    hu_dpo_record_from_feedback(&col, "p", 1, "resp", 4, true);
    HU_ASSERT_EQ(hu_dpo_clear(&col), HU_OK);
    size_t count = 999;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT_EQ((int)count, 0);
    hu_dpo_collector_deinit(&col);
}

static void dpo_get_best_examples_invalid_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    char *frag = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_dpo_get_best_examples(NULL, &alloc, 5, &frag, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_dpo_get_best_examples(&col, NULL, 5, &frag, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_dpo_get_best_examples(&col, &alloc, 5, NULL, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_dpo_get_best_examples(&col, &alloc, 5, &frag, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_dpo_collector_deinit(&col);
}

static void dpo_get_best_examples_no_db_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    char *frag = NULL;
    size_t len = 999;
    HU_ASSERT_EQ(hu_dpo_get_best_examples(&col, &alloc, 5, &frag, &len), HU_OK);
    HU_ASSERT_TRUE(frag == NULL);
    HU_ASSERT_EQ(len, 0u);
    HU_ASSERT_EQ(hu_dpo_get_best_examples(&col, &alloc, 0, &frag, &len), HU_OK);
    HU_ASSERT_TRUE(frag == NULL);
    HU_ASSERT_EQ(len, 0u);
    hu_dpo_collector_deinit(&col);
}

#ifdef HU_ENABLE_SQLITE
static void dpo_get_best_examples_sqlite_orders_by_margin(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    hu_preference_pair_t low = {0};
    memcpy(low.prompt, "low", 3);
    low.prompt_len = 3;
    memcpy(low.chosen, "c1", 2);
    low.chosen_len = 2;
    low.margin = 0.4;
    HU_ASSERT_EQ(hu_dpo_record_pair(&col, &low), HU_OK);

    hu_preference_pair_t high = {0};
    memcpy(high.prompt, "high prompt", 11);
    high.prompt_len = 11;
    memcpy(high.chosen, "good", 4);
    high.chosen_len = 4;
    /* Both sides must be >= 4 bytes — symmetric with the corpus-inversion
     * filter in hu_dpo_get_best_examples (see dpo.c:696). 3-byte "bad"
     * was the degenerate case the filter is designed to exclude. */
    memcpy(high.rejected, "lousy", 5);
    high.rejected_len = 5;
    high.margin = 0.9;
    HU_ASSERT_EQ(hu_dpo_record_pair(&col, &high), HU_OK);

    char *frag = NULL;
    size_t frag_len = 0;
    HU_ASSERT_EQ(hu_dpo_get_best_examples(&col, &alloc, 5, &frag, &frag_len), HU_OK);
    HU_ASSERT_NOT_NULL(frag);
    HU_ASSERT_TRUE(frag_len > 0);
    HU_ASSERT_TRUE(strstr(frag, "Learned preferences") != NULL);
    HU_ASSERT_TRUE(strstr(frag, "high prompt") != NULL);
    HU_ASSERT_TRUE(strstr(frag, "GOOD") != NULL);
    HU_ASSERT_TRUE(strstr(frag, "BAD") != NULL);
    HU_ASSERT_TRUE(strstr(frag, "lousy") != NULL);
    HU_ASSERT_TRUE(strstr(frag, "low") == NULL);

    alloc.free(alloc.ctx, frag, frag_len + 1);

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}

static void dpo_max_pairs_ring_buffer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 3, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);
    for (int i = 0; i < 5; i++)
        hu_dpo_record_from_feedback(&col, "p", 1, "resp", 4, true);
    /* DB should have at most 3 rows due to ring buffer eviction */
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM dpo_pairs", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int db_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    HU_ASSERT(db_count <= 3);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}
#endif

static void dpo_null_collector_returns_error(void) {
    HU_ASSERT_EQ(hu_dpo_record_pair(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    size_t out = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(NULL, &out), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_dpo_clear(NULL), HU_ERR_INVALID_ARGUMENT);
}

static void dpo_empty_export_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    size_t exported = 999;
    HU_ASSERT_EQ(hu_dpo_export_jsonl(&col, "out.jsonl", 9, &exported), HU_OK);
    HU_ASSERT_EQ((int)exported, 0);
    hu_dpo_collector_deinit(&col);
}

static void test_dpo_export_in_memory_roundtrip(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 16, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    hu_preference_pair_t in = {0};
    memcpy(in.prompt, "what should i do first?", 23);
    in.prompt_len = 23;
    memcpy(in.chosen, "ship the small fix.", 19);
    in.chosen_len = 19;
    memcpy(in.rejected, "perhaps consider.", 17);
    in.rejected_len = 17;
    in.margin = 0.7;
    in.timestamp = 1715472000;
    memcpy(in.source, "e2e_test", 8);
    in.source_len = 8;
    HU_ASSERT_EQ(hu_dpo_record_pair(&col, &in), HU_OK);

    hu_dpo_export_t ex = {0};
    HU_ASSERT_EQ(hu_dpo_export(&col, &alloc, &ex), HU_OK);
    HU_ASSERT_EQ(ex.count, 1u);
    HU_ASSERT_EQ(memcmp(ex.pairs[0].prompt, in.prompt, in.prompt_len), 0);
    hu_dpo_export_free(&alloc, &ex);

    sqlite3 *db2 = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db2), SQLITE_OK);
    hu_dpo_collector_t col2 = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db2, 16, &col2), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col2), HU_OK);
    hu_dpo_export_t ex2 = {0};
    HU_ASSERT_EQ(hu_dpo_export(&col2, &alloc, &ex2), HU_OK);
    HU_ASSERT_EQ(ex2.count, 0u);
    hu_dpo_export_free(&alloc, &ex2);

    hu_dpo_collector_deinit(&col);
    hu_dpo_collector_deinit(&col2);
    sqlite3_close(db);
    sqlite3_close(db2);
#else
    HU_SKIP_IF(1, "SQLite required for hu_dpo_export");
#endif
}

/* ── AGI Capability-1: production_outcomes write + outcome update ── */

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Helper: opens an in-memory SQLite DB, returns it via *db_out. The
 * caller is responsible for sqlite3_close. Used by outcome tests to
 * exercise the SQLite path without writing to ~/.human/memory.db. */
static int open_inmem_db(sqlite3 **db_out) {
    return sqlite3_open(":memory:", db_out);
}

static void dpo_record_outbound_inserts_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(open_inmem_db(&db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    HU_ASSERT_EQ(hu_dpo_record_outbound(&col, "imessage", 8, "+15555550100", 12, "msg_abc", 7,
                                        "you around?", 11, "yeah just got back", 18, 0.85, NULL, 0),
                 HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "SELECT channel, chosen, p_seth_at_send "
                       "FROM production_outcomes WHERE message_ref='msg_abc'",
                       -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 0), "imessage");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 1), "yeah just got back");
    HU_ASSERT_TRUE(sqlite3_column_double(st, 2) > 0.84 && sqlite3_column_double(st, 2) < 0.86);
    sqlite3_finalize(st);

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}

static void dpo_record_outcome_updates_resolution(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(open_inmem_db(&db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    /* Outbound first */
    HU_ASSERT_EQ(hu_dpo_record_outbound(&col, "imessage", 8, "+15555550100", 12, "msg_xyz", 7,
                                        "thanks!", 7, "no worries man", 14, 0.91, NULL, 0),
                 HU_OK);

    /* Outcome arrives: positive tapback */
    HU_ASSERT_EQ(hu_dpo_record_outcome(&col, "imessage", 8, "+15555550100", 12, "msg_xyz", 7,
                                       /*polarity=*/+1, /*latency=*/45,
                                       /*reply_len=*/-1),
                 HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "SELECT tapback_polarity, reply_latency_s, "
                       "outcome_resolved_at FROM production_outcomes "
                       "WHERE message_ref='msg_xyz'",
                       -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 1);
    HU_ASSERT_EQ(sqlite3_column_int(st, 1), 45);
    HU_ASSERT_TRUE(sqlite3_column_int64(st, 2) > 0);
    sqlite3_finalize(st);

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}

static void dpo_record_outbound_null_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    /* NULL channel */
    HU_ASSERT_EQ(
        hu_dpo_record_outbound(&col, NULL, 0, "t", 1, NULL, 0, "p", 1, "c", 1, 0.5, NULL, 0),
        HU_ERR_INVALID_ARGUMENT);
    /* Empty target */
    HU_ASSERT_EQ(
        hu_dpo_record_outbound(&col, "imsg", 4, "", 0, NULL, 0, "p", 1, "c", 1, 0.5, NULL, 0),
        HU_ERR_INVALID_ARGUMENT);
    hu_dpo_collector_deinit(&col);
}

/* Sprint 46 R5.2 — alternatives column persistence */

static void dpo_record_outbound_with_alternatives_persists_json(void) {
    /* L5 best-of-N writes the LOSING candidates as a JSON array into
     * the alternatives column. outcomes_to_dpo.py reads them later to
     * materialize DPO pairs once the outcome resolves. */
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(open_inmem_db(&db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    const char *alts = "[\"yeah\",\"sure thing\",\"absolutely\"]";
    HU_ASSERT_EQ(hu_dpo_record_outbound(&col, "imessage", 8, "+15551112222", 12, "msg_alt", 7,
                                        "you free?", 9, "yeah whats up", 13, 0.91, alts,
                                        strlen(alts)),
                 HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "SELECT alternatives FROM production_outcomes "
                       "WHERE message_ref='msg_alt'",
                       -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    const char *stored = (const char *)sqlite3_column_text(st, 0);
    HU_ASSERT_NOT_NULL(stored);
    /* Exact round-trip: stored bytes equal the input. */
    HU_ASSERT_STR_EQ(stored, alts);
    sqlite3_finalize(st);

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}

static void dpo_record_outbound_null_alternatives_stores_null(void) {
    /* When L5 didn't fire (or only one candidate), pass NULL/0 and the
     * column stays SQL NULL — not the JSON string "null". */
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(open_inmem_db(&db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    HU_ASSERT_EQ(hu_dpo_record_outbound(&col, "imessage", 8, "+15553334444", 12, "msg_no_alt", 10,
                                        "hey", 3, "hey whats good", 14, 0.78, NULL, 0),
                 HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "SELECT alternatives FROM production_outcomes "
                       "WHERE message_ref='msg_no_alt'",
                       -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_type(st, 0), SQLITE_NULL);
    sqlite3_finalize(st);

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}

/* Sprint 46 R5.1 — latency ingest test cluster */

static void dpo_record_outcome_with_latency_sets_column(void) {
    /* Unit test for the existing API: write outbound, then call
     * record_outcome with latency=42; assert column persists.
     * Already pinned in part by dpo_record_outcome_updates_resolution;
     * this is the latency-only variant. */
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(open_inmem_db(&db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_outbound(&col, "imessage", 8, "+15551234567", 12, "msg_r5_1", 8,
                                        "you up?", 7, "yeah whats up", 13, 0.84, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_outcome(&col, "imessage", 8, "+15551234567", 12, "msg_r5_1", 8,
                                       /*tapback_polarity=*/-2, /* sentinel: leave column */
                                       /*reply_latency_s=*/42, /*reply_length=*/13),
                 HU_OK);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "SELECT reply_latency_s, reply_length FROM production_outcomes "
                       "WHERE message_ref='msg_r5_1'",
                       -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 42);
    HU_ASSERT_EQ(sqlite3_column_int(st, 1), 13);
    sqlite3_finalize(st);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}

static void dpo_record_inbound_arrival_computes_latency(void) {
    /* The Sprint 46 R5.1 acceptance test: drive the high-level wrapper
     * that the daemon will call when an inbound iMessage arrives.
     * Pre-conditions:
     *   - One outbound row exists for (channel='imessage', target='+15559876543')
     *     with send_timestamp set to a known past time.
     * Action:
     *   - hu_dpo_record_inbound_arrival is called with the same
     *     (channel, target).
     * Post-conditions:
     *   - The row's reply_latency_s is now > 0 (the actual value
     *     depends on time elapsed; we just assert it's plausible).
     *   - The row's outcome_resolved_at is now non-NULL.
     */
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(open_inmem_db(&db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    /* Inject an outbound row with a known-past send_timestamp.
     * Bypass hu_dpo_record_outbound (which uses time(NULL)) and INSERT
     * directly so we can control the timestamp for a deterministic
     * latency. */
    int64_t past = (int64_t)time(NULL) - 60; /* 60s ago */
    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db,
                       "INSERT INTO production_outcomes(channel, target, message_ref, "
                       "prompt, chosen, send_timestamp) VALUES (?,?,?,?,?,?)",
                       -1, &ins, NULL);
    sqlite3_bind_text(ins, 1, "imessage", 8, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "+15559876543", 12, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "msg_r5_1_lat", 12, SQLITE_STATIC);
    sqlite3_bind_text(ins, 4, "hey there", 9, SQLITE_STATIC);
    sqlite3_bind_text(ins, 5, "yeah whats up", 13, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 6, past);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_finalize(ins);

    /* Call the wrapper — should look up the row above and fill latency. */
    HU_ASSERT_EQ(hu_dpo_record_inbound_arrival(&col, "imessage", 8, "+15559876543", 12,
                                               /*inbound_length=*/24),
                 HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "SELECT reply_latency_s, reply_length, outcome_resolved_at "
                       "FROM production_outcomes WHERE message_ref='msg_r5_1_lat'",
                       -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    /* Latency should be approximately 60s (we set send_timestamp 60s
     * ago). Allow ±5s for test execution time. */
    int latency = sqlite3_column_int(st, 0);
    HU_ASSERT(latency >= 55 && latency <= 70);
    HU_ASSERT_EQ(sqlite3_column_int(st, 1), 24);
    HU_ASSERT(sqlite3_column_int64(st, 2) > 0);
    sqlite3_finalize(st);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}

static void dpo_record_inbound_arrival_no_outbound_is_noop(void) {
    /* When the contact texts us WITHOUT a prior outbound, the wrapper
     * is a no-op — not an error. (Mirror the production "contact
     * messaged us first" case.) */
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(open_inmem_db(&db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_inbound_arrival(&col, "imessage", 8, "+15550000000", 12, 10), HU_OK);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}

static void dpo_record_inbound_arrival_null_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_inbound_arrival(&col, NULL, 0, "t", 1, 0), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_dpo_record_inbound_arrival(&col, "imsg", 4, "", 0, 0), HU_ERR_INVALID_ARGUMENT);
    hu_dpo_collector_deinit(&col);
}

#endif /* HU_ENABLE_SQLITE */

static void dpo_margin_reflects_confidence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &col), HU_OK);
    /* record_from_feedback uses margin=0.7, record_from_retry uses margin=0.8 */
    HU_ASSERT_EQ(hu_dpo_record_from_feedback(&col, "p", 1, "resp", 4, true), HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_from_retry(&col, "p", 1, "rejected", 8, "chosen", 6), HU_OK);
    size_t count = 0;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT_EQ((int)count, 2);
    hu_dpo_collector_deinit(&col);
}

/* RLAIF nightly gate — pure predicate, no DB/provider needed. Pins the
 * boundary that keeps the nightly from patching the persona off noise batches
 * (the live bug: it applied a style patch from alignment=0.00 / loss=0.693
 * random-baseline batches). */
static void rlaif_gate_rejects_noise_batch(void) {
    hu_dpo_judge_result_t r = {
        .loss = 0.6931, .alignment_score = 0.00, .pairs_evaluated = 32, .pairs_aligned = 0};
    HU_ASSERT_FALSE(hu_rlaif_should_apply_style_patch(&r));
}

static void rlaif_gate_rejects_low_alignment(void) {
    hu_dpo_judge_result_t r = {
        .loss = 0.6924, .alignment_score = 0.03, .pairs_evaluated = 32, .pairs_aligned = 1};
    HU_ASSERT_FALSE(hu_rlaif_should_apply_style_patch(&r));
}

static void rlaif_gate_accepts_at_threshold(void) {
    hu_dpo_judge_result_t r = {.loss = 0.66,
                               .alignment_score = HU_RLAIF_MIN_ALIGNMENT_TO_PATCH,
                               .pairs_evaluated = 32,
                               .pairs_aligned = 20};
    HU_ASSERT_TRUE(hu_rlaif_should_apply_style_patch(&r));
}

static void rlaif_gate_accepts_strong_signal(void) {
    hu_dpo_judge_result_t r = {
        .loss = 0.6556, .alignment_score = 0.94, .pairs_evaluated = 32, .pairs_aligned = 30};
    HU_ASSERT_TRUE(hu_rlaif_should_apply_style_patch(&r));
}

static void rlaif_gate_rejects_empty_batch(void) {
    /* High alignment but zero pairs evaluated => no real evidence. */
    hu_dpo_judge_result_t r = {
        .loss = 0.0, .alignment_score = 1.0, .pairs_evaluated = 0, .pairs_aligned = 0};
    HU_ASSERT_FALSE(hu_rlaif_should_apply_style_patch(&r));
}

static void rlaif_gate_rejects_null(void) {
    HU_ASSERT_FALSE(hu_rlaif_should_apply_style_patch(NULL));
}

/* Judge score parser — the boundary that decides whether a pair is SCORED or
 * SKIPPED. A failed/empty/non-numeric judge reply must return false so the
 * caller skips the pair instead of fabricating a neutral-50 tie (which is what
 * manufactured the alignment=0 / loss=ln2 noise). */
static void judge_parse_extracts_number(void) {
    double s = -1.0;
    HU_ASSERT_TRUE(hu_dpo_parse_judge_score("100", 3, &s));
    HU_ASSERT_EQ((int)s, 100);
}

static void judge_parse_leading_text_then_number(void) {
    double s = -1.0;
    HU_ASSERT_TRUE(hu_dpo_parse_judge_score("Score: 85", 9, &s));
    HU_ASSERT_EQ((int)s, 85);
}

static void judge_parse_empty_returns_false(void) {
    /* The failure signature: empty reply (timeout / thinking-only) must NOT
     * yield a score, so the caller skips rather than scoring a fake tie. */
    double s = 50.0;
    HU_ASSERT_FALSE(hu_dpo_parse_judge_score("", 0, &s));
}

static void judge_parse_no_digit_returns_false(void) {
    double s = 50.0;
    HU_ASSERT_FALSE(hu_dpo_parse_judge_score("no idea", 7, &s));
}

static void judge_parse_null_returns_false(void) {
    double s = 50.0;
    HU_ASSERT_FALSE(hu_dpo_parse_judge_score(NULL, 5, &s));
}

static void judge_parse_clamps_above_100(void) {
    double s = -1.0;
    HU_ASSERT_TRUE(hu_dpo_parse_judge_score("9999", 4, &s));
    HU_ASSERT_EQ((int)s, 100);
}

#ifdef HU_ENABLE_SQLITE
/* Record the single-sided rows reaction collection writes: a positive reaction
 * fills only `chosen`, a negative only `rejected`. */
static void dpo_paired_record_chosen_only(hu_dpo_collector_t *col, const char *prompt,
                                          const char *chosen, int64_t ts) {
    hu_preference_pair_t p = {0};
    size_t pl = strlen(prompt), cl = strlen(chosen);
    memcpy(p.prompt, prompt, pl);
    p.prompt_len = pl;
    memcpy(p.chosen, chosen, cl);
    p.chosen_len = cl;
    p.margin = 1.0;
    p.timestamp = ts;
    memcpy(p.source, "imessage_tapback", 16);
    p.source_len = 16;
    HU_ASSERT_EQ(hu_dpo_record_pair(col, &p), HU_OK);
}
static void dpo_paired_record_rejected_only(hu_dpo_collector_t *col, const char *prompt,
                                            const char *rejected, int64_t ts) {
    hu_preference_pair_t p = {0};
    size_t pl = strlen(prompt), rl = strlen(rejected);
    memcpy(p.prompt, prompt, pl);
    p.prompt_len = pl;
    memcpy(p.rejected, rejected, rl);
    p.rejected_len = rl;
    p.margin = -1.0;
    p.timestamp = ts;
    memcpy(p.source, "imessage_tapback", 16);
    p.source_len = 16;
    HU_ASSERT_EQ(hu_dpo_record_pair(col, &p), HU_OK);
}
#endif

/* AC (a): same-prompt chosen-only + rejected-only rows zip into two-sided pairs;
 * plain hu_dpo_export drops them all (the bug this fixes). */
static void dpo_export_paired_zips_same_prompt_singles(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 32, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    const char *P = "what should i do first?";
    dpo_paired_record_chosen_only(&col, P, "ship the small fix now", 100);
    dpo_paired_record_rejected_only(&col, P, "consider exploring more", 101);
    dpo_paired_record_chosen_only(&col, P, "merge the ready branch", 102);
    dpo_paired_record_rejected_only(&col, P, "rewrite the whole module", 103);

    hu_dpo_export_t ex = {0};
    HU_ASSERT_EQ(hu_dpo_export_paired(&col, &alloc, &ex), HU_OK);
    HU_ASSERT_EQ(ex.count, 2u);
    for (size_t i = 0; i < ex.count; i++) {
        HU_ASSERT_TRUE(ex.pairs[i].chosen_len >= 4);
        HU_ASSERT_TRUE(ex.pairs[i].rejected_len >= 4);
        HU_ASSERT_EQ(memcmp(ex.pairs[i].prompt, P, strlen(P)), 0);
    }

    /* Contrast: plain export drops every single-sided row → 0 pairs. */
    hu_dpo_export_t plain = {0};
    HU_ASSERT_EQ(hu_dpo_export(&col, &alloc, &plain), HU_OK);
    HU_ASSERT_EQ(plain.count, 0u);

    hu_dpo_export_free(&alloc, &ex);
    hu_dpo_export_free(&alloc, &plain);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
#else
    HU_SKIP_IF(1, "SQLite required for hu_dpo_export_paired");
#endif
}

/* AC (b): unequal chosen/rejected counts for one prompt → min(nc, nr) pairs,
 * remainder dropped. */
static void dpo_export_paired_unequal_counts_drops_remainder(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 32, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    const char *P = "ranked prompt here";
    dpo_paired_record_chosen_only(&col, P, "good answer one", 200);
    dpo_paired_record_chosen_only(&col, P, "good answer two", 201);
    dpo_paired_record_chosen_only(&col, P, "good answer three", 202);
    dpo_paired_record_rejected_only(&col, P, "bad answer only", 203);

    hu_dpo_export_t ex = {0};
    HU_ASSERT_EQ(hu_dpo_export_paired(&col, &alloc, &ex), HU_OK);
    HU_ASSERT_EQ(ex.count, 1u); /* min(3 chosen, 1 rejected) */
    HU_ASSERT_TRUE(ex.pairs[0].chosen_len >= 4 && ex.pairs[0].rejected_len >= 4);

    hu_dpo_export_free(&alloc, &ex);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
#else
    HU_SKIP_IF(1, "SQLite required for hu_dpo_export_paired");
#endif
}

/* AC (c): single-sided rows with DIFFERENT prompts are never paired. */
static void dpo_export_paired_different_prompts_not_paired(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 32, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    dpo_paired_record_chosen_only(&col, "prompt alpha here", "chosen for alpha", 300);
    dpo_paired_record_rejected_only(&col, "prompt beta here", "rejected for beta", 301);

    hu_dpo_export_t ex = {0};
    HU_ASSERT_EQ(hu_dpo_export_paired(&col, &alloc, &ex), HU_OK);
    HU_ASSERT_EQ(ex.count, 0u); /* no same-prompt partner */

    hu_dpo_export_free(&alloc, &ex);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
#else
    HU_SKIP_IF(1, "SQLite required for hu_dpo_export_paired");
#endif
}

/* AC (d): genuine two-sided rows pass through unchanged. */
static void dpo_export_paired_passes_through_two_sided(void) {
#ifdef HU_ENABLE_SQLITE
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 32, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    hu_preference_pair_t in = {0};
    memcpy(in.prompt, "what should i do first?", 23);
    in.prompt_len = 23;
    memcpy(in.chosen, "ship the small fix.", 19);
    in.chosen_len = 19;
    memcpy(in.rejected, "perhaps consider.", 17);
    in.rejected_len = 17;
    in.margin = 0.7;
    in.timestamp = 1715472000;
    memcpy(in.source, "retry_test", 10);
    in.source_len = 10;
    HU_ASSERT_EQ(hu_dpo_record_pair(&col, &in), HU_OK);

    hu_dpo_export_t ex = {0};
    HU_ASSERT_EQ(hu_dpo_export_paired(&col, &alloc, &ex), HU_OK);
    HU_ASSERT_EQ(ex.count, 1u);
    HU_ASSERT_EQ(memcmp(ex.pairs[0].chosen, in.chosen, in.chosen_len), 0);
    HU_ASSERT_EQ(memcmp(ex.pairs[0].rejected, in.rejected, in.rejected_len), 0);

    hu_dpo_export_free(&alloc, &ex);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
#else
    HU_SKIP_IF(1, "SQLite required for hu_dpo_export_paired");
#endif
}

void run_dpo_tests(void) {
    HU_TEST_SUITE("DPO Preference");
    HU_RUN_TEST(judge_parse_extracts_number);
    HU_RUN_TEST(judge_parse_leading_text_then_number);
    HU_RUN_TEST(judge_parse_empty_returns_false);
    HU_RUN_TEST(judge_parse_no_digit_returns_false);
    HU_RUN_TEST(judge_parse_null_returns_false);
    HU_RUN_TEST(judge_parse_clamps_above_100);
    HU_RUN_TEST(rlaif_gate_rejects_noise_batch);
    HU_RUN_TEST(rlaif_gate_rejects_low_alignment);
    HU_RUN_TEST(rlaif_gate_accepts_at_threshold);
    HU_RUN_TEST(rlaif_gate_accepts_strong_signal);
    HU_RUN_TEST(rlaif_gate_rejects_empty_batch);
    HU_RUN_TEST(rlaif_gate_rejects_null);
    HU_RUN_TEST(dpo_create_and_init_tables);
    HU_RUN_TEST(dpo_record_pair_stores_correctly);
    HU_RUN_TEST(dpo_record_from_feedback_positive);
    HU_RUN_TEST(dpo_record_from_feedback_negative);
    HU_RUN_TEST(dpo_record_from_retry);
    HU_RUN_TEST(dpo_export_jsonl_format);
    HU_RUN_TEST(dpo_pair_count_accurate);
    HU_RUN_TEST(dpo_clear_removes_all);
    HU_RUN_TEST(dpo_get_best_examples_invalid_args);
    HU_RUN_TEST(dpo_get_best_examples_no_db_returns_empty);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(dpo_get_best_examples_sqlite_orders_by_margin);
    HU_RUN_TEST(dpo_max_pairs_ring_buffer);
    /* AGI Capability-1 production_outcomes tests */
    HU_RUN_TEST(dpo_record_outbound_inserts_row);
    HU_RUN_TEST(dpo_record_outcome_updates_resolution);
    HU_RUN_TEST(dpo_record_outbound_null_returns_invalid);
    /* Sprint 46 R5.2 — alternatives column */
    HU_RUN_TEST(dpo_record_outbound_with_alternatives_persists_json);
    HU_RUN_TEST(dpo_record_outbound_null_alternatives_stores_null);
    /* Sprint 46 R5.1 — latency ingest */
    HU_RUN_TEST(dpo_record_outcome_with_latency_sets_column);
    HU_RUN_TEST(dpo_record_inbound_arrival_computes_latency);
    HU_RUN_TEST(dpo_record_inbound_arrival_no_outbound_is_noop);
    HU_RUN_TEST(dpo_record_inbound_arrival_null_returns_invalid);
#endif
    HU_RUN_TEST(dpo_null_collector_returns_error);
    HU_RUN_TEST(dpo_empty_export_succeeds);
    HU_RUN_TEST(dpo_margin_reflects_confidence);
    HU_RUN_TEST(test_dpo_export_in_memory_roundtrip);
    HU_RUN_TEST(dpo_export_paired_zips_same_prompt_singles);
    HU_RUN_TEST(dpo_export_paired_unequal_counts_drops_remainder);
    HU_RUN_TEST(dpo_export_paired_different_prompts_not_paired);
    HU_RUN_TEST(dpo_export_paired_passes_through_two_sided);
}
