/* tests/test_tom_activation.c
 *
 * Phase A of specs/2026-05-19-tom-activation: pins AC-TOM-1, AC-TOM-2,
 * AC-TOM-3 and Task 11 (GC). The in-memory hu_tom_record_user_expectation
 * path is covered by tests/test_mutual_tom.c; this file exercises the
 * new SQLite-backed persistence + the build_context_with_expectations
 * surface + the GC tick.
 */

#include "human/agent/theory_of_mind.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

static sqlite3 *open_in_memory_db(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_tom_user_expectations_init_table(db), HU_OK);
    return db;
}

static void close_db(sqlite3 *db) {
    if (db)
        sqlite3_close(db);
}

/* ── AC-TOM-1: detector pulls "as you know" out of inbound text ─────── */

static void test_tom_detect_user_expectation_fires_on_as_you_know_phrase(void) {
    const char *text = "as you know I prefer concise replies";
    size_t text_len = strlen(text);
    const char *topic = NULL;
    size_t topic_len = 0;
    hu_tom_expected_knowledge_t ktype = HU_TOM_EXPECT_REMEMBERS;

    bool found = hu_tom_detect_user_expectation(text, text_len, &topic, &topic_len, &ktype);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_NOT_NULL(topic);
    HU_ASSERT_GT(topic_len, 0u);
    /* Topic extraction grabs the clause after the pattern; the existing
     * pattern table says "as you know" → UNDERSTANDS. The contract is
     * "topic is present and extracted"; we don't care about the exact
     * extraction substring boundary here because pattern tuning is an
     * explicit non-goal of this spec. */
    HU_ASSERT_EQ((int)ktype, (int)HU_TOM_EXPECT_UNDERSTANDS);
    /* Sanity-check the topic substring covers the meaningful clause. */
    char extracted[128] = {0};
    size_t copy = topic_len < sizeof(extracted) - 1 ? topic_len : sizeof(extracted) - 1;
    memcpy(extracted, topic, copy);
    /* "concise replies" must appear inside the extracted topic span. */
    HU_ASSERT_NOT_NULL(strstr(extracted, "concise replies"));

    /* Now confirm the persistence path can capture this through the
     * public API (this is the Task 1 + Task 2 union — what the daemon
     * site does on every inbound message). */
    sqlite3 *db = open_in_memory_db();
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "contact_a", topic, topic_len, ktype, "sess1",
                                                 5, /*turn*/ 0, /*now_ms*/ 1000),
                 HU_OK);
    int64_t cnt = 0;
    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "contact_a", &cnt), HU_OK);
    HU_ASSERT_EQ((int)cnt, 1);
    close_db(db);
}

/* ── AC-TOM-2: idempotent on UNIQUE (contact_id, topic, session_key) ──── */

static void test_tom_record_user_expectation_idempotent_on_unique_tuple(void) {
    sqlite3 *db = open_in_memory_db();

    /* First insert. */
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "contact_b", "birthday", 8,
                                                 HU_TOM_EXPECT_REMEMBERS, "sess1", 5, 0, 1000),
                 HU_OK);
    /* Same (contact, topic, session_key) again — INSERT OR IGNORE must
     * preserve count at 1. */
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "contact_b", "birthday", 8,
                                                 HU_TOM_EXPECT_REMEMBERS, "sess1", 5, 0, 2000),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "contact_b", "birthday", 8,
                                                 HU_TOM_EXPECT_REMEMBERS, "sess1", 5, 0, 3000),
                 HU_OK);

    int64_t cnt = 0;
    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "contact_b", &cnt), HU_OK);
    HU_ASSERT_EQ((int)cnt, 1);

    /* Different session_key → distinct row. */
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "contact_b", "birthday", 8,
                                                 HU_TOM_EXPECT_REMEMBERS, "sess2", 5, 0, 4000),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "contact_b", &cnt), HU_OK);
    HU_ASSERT_EQ((int)cnt, 2);

    /* Different contact → distinct row. */
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "contact_c", "birthday", 8,
                                                 HU_TOM_EXPECT_REMEMBERS, "sess1", 5, 0, 5000),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "contact_b", &cnt), HU_OK);
    HU_ASSERT_EQ((int)cnt, 2);
    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "contact_c", &cnt), HU_OK);
    HU_ASSERT_EQ((int)cnt, 1);

    close_db(db);
}

/* ── AC-TOM-3: prompt build includes Unmet section when expectation has
 *               no matching belief. ───────────────────────────────────── */

static void test_tom_build_context_includes_unmet_expectations_section(void) {
    sqlite3 *db = open_in_memory_db();
    hu_allocator_t alloc = hu_system_allocator();

    /* One persisted expectation that the belief state knows NOTHING about. */
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "contact_d", "concise replies", 15,
                                                 HU_TOM_EXPECT_UNDERSTANDS, "sess1", 5, 0, 1000),
                 HU_OK);

    hu_tom_persisted_expectation_t *exps = NULL;
    size_t exp_count = 0;
    HU_ASSERT_EQ(
        hu_tom_user_expectations_load_unresolved(db, &alloc, "contact_d", 8, &exps, &exp_count),
        HU_OK);
    HU_ASSERT_EQ((int)exp_count, 1);
    HU_ASSERT_STR_EQ(exps[0].topic, "concise replies");

    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));
    HU_ASSERT_EQ(hu_tom_init(&state, &alloc, "contact_d", 9), HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(
        hu_tom_build_context_with_expectations(&state, exps, exp_count, &alloc, &ctx, &ctx_len),
        HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_STR_CONTAINS(ctx, "### Unmet User Expectations");
    HU_ASSERT_STR_CONTAINS(ctx, "concise replies");
    HU_ASSERT_STR_CONTAINS(ctx, "understands");

    hu_str_free(&alloc, ctx);
    hu_tom_persisted_expectations_free(&alloc, exps, exp_count);
    hu_tom_deinit(&state, &alloc);
    close_db(db);
}

/* ── AC-TOM-3 negative: omit section when no expectations. ────────────── */

static void test_tom_build_context_omits_section_when_no_unmet(void) {
    sqlite3 *db = open_in_memory_db();
    hu_allocator_t alloc = hu_system_allocator();

    hu_tom_persisted_expectation_t *exps = NULL;
    size_t exp_count = 0;
    HU_ASSERT_EQ(
        hu_tom_user_expectations_load_unresolved(db, &alloc, "contact_e", 8, &exps, &exp_count),
        HU_OK);
    HU_ASSERT_EQ((int)exp_count, 0);
    HU_ASSERT_NULL(exps);

    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));
    HU_ASSERT_EQ(hu_tom_init(&state, &alloc, "contact_e", 9), HU_OK);
    /* Add a single belief so the base "### Contact Mental Model" block
     * has content — we are verifying that the UNMET section is OMITTED
     * cleanly, not that the whole context is empty. */
    HU_ASSERT_EQ(hu_tom_record_belief(&state, &alloc, "uses iTerm", 10, HU_BELIEF_KNOWS, 0.8f),
                 HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_tom_build_context_with_expectations(&state, NULL, 0, &alloc, &ctx, &ctx_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_STR_CONTAINS(ctx, "### Contact Mental Model");
    /* The new section must NOT be present (no empty header). */
    HU_ASSERT_STR_NOT_CONTAINS(ctx, "### Unmet User Expectations");

    hu_str_free(&alloc, ctx);
    hu_tom_deinit(&state, &alloc);
    close_db(db);
}

/* ── Task 11: GC deletes resolved rows older than ttl_ms. ──────────────── */

/* Helper to manually mark a row resolved via direct UPDATE — Phase A
 * doesn't ship the resolution path (Q-TOM-B deferred to Phase B). The
 * test harness sets resolved_ts_ms by hand to simulate the future
 * resolution-loop output. */
static void mark_resolved(sqlite3 *db, const char *contact, const char *topic, int64_t ts_ms) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "UPDATE tom_user_expectations SET resolved_ts_ms = ? "
                                "WHERE contact_id = ? AND topic = ?",
                                -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, ts_ms);
    sqlite3_bind_text(stmt, 2, contact, (int)strlen(contact), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, topic, (int)strlen(topic), SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void test_tom_expectation_gc_deletes_old_resolved(void) {
    sqlite3 *db = open_in_memory_db();

    const int64_t now_ms = 1000LL * 86400 * 1000; /* arbitrary "now" */
    const int64_t ttl_ms = 30LL * 86400 * 1000;   /* 30 days */

    /* Insert + immediately mark resolved 31 days ago. */
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "contact_f", "stale topic", 11,
                                                 HU_TOM_EXPECT_REMEMBERS, "sess", 4, 0,
                                                 now_ms - (32LL * 86400 * 1000)),
                 HU_OK);
    mark_resolved(db, "contact_f", "stale topic", now_ms - (31LL * 86400 * 1000));

    int64_t cnt = 0;
    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "contact_f", &cnt), HU_OK);
    HU_ASSERT_EQ((int)cnt, 1);

    int64_t deleted = 0;
    HU_ASSERT_EQ(hu_tom_user_expectations_gc(db, now_ms, ttl_ms, &deleted), HU_OK);
    HU_ASSERT_EQ((int)deleted, 1);

    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "contact_f", &cnt), HU_OK);
    HU_ASSERT_EQ((int)cnt, 0);

    close_db(db);
}

/* ── Task 11 negative: GC preserves recent-resolved and unresolved. ─── */

static void test_tom_expectation_gc_preserves_recent_or_unresolved(void) {
    sqlite3 *db = open_in_memory_db();

    const int64_t now_ms = 1000LL * 86400 * 1000;
    const int64_t ttl_ms = 30LL * 86400 * 1000;

    /* Unresolved row — should never be deleted by GC. */
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "contact_g", "open question", 13,
                                                 HU_TOM_EXPECT_REMEMBERS, "sess", 4, 0,
                                                 now_ms - (60LL * 86400 * 1000)),
                 HU_OK);
    /* Recently-resolved row (1 day ago) — also preserved. */
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "contact_g", "recent topic", 12,
                                                 HU_TOM_EXPECT_REMEMBERS, "sess2", 5, 0,
                                                 now_ms - (5LL * 86400 * 1000)),
                 HU_OK);
    mark_resolved(db, "contact_g", "recent topic", now_ms - (1LL * 86400 * 1000));

    int64_t cnt = 0;
    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "contact_g", &cnt), HU_OK);
    HU_ASSERT_EQ((int)cnt, 2);

    int64_t deleted = 0;
    HU_ASSERT_EQ(hu_tom_user_expectations_gc(db, now_ms, ttl_ms, &deleted), HU_OK);
    HU_ASSERT_EQ((int)deleted, 0);

    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "contact_g", &cnt), HU_OK);
    HU_ASSERT_EQ((int)cnt, 2);

    close_db(db);
}

/* ── Bonus: daemon-tick wrapper enforces the interval gate. ─────────── */

static void test_tom_expectation_gc_tick_respects_interval(void) {
    sqlite3 *db = open_in_memory_db();
    hu_daemon_tick_tom_expectation_gc_reset_warn_guards_for_test();

    const int64_t now_ms = 1000LL * 86400 * 1000;
    const int64_t ttl_ms = 30LL * 86400 * 1000;
    /* 24h interval. */
    const int64_t interval_sec = 86400;

    int64_t last_run = 0;
    /* First tick — runs because watermark is 0. */
    HU_ASSERT_EQ(hu_daemon_tick_tom_expectation_gc(db, now_ms, &last_run, interval_sec, ttl_ms),
                 HU_OK);
    HU_ASSERT_EQ(last_run, now_ms);

    /* Second tick 1 hour later — skipped. */
    int64_t now2 = now_ms + (3600LL * 1000);
    HU_ASSERT_EQ(hu_daemon_tick_tom_expectation_gc(db, now2, &last_run, interval_sec, ttl_ms),
                 HU_OK);
    /* last_run was NOT updated — the interval gate suppressed the run. */
    HU_ASSERT_EQ(last_run, now_ms);

    /* Third tick 25 hours later — runs. */
    int64_t now3 = now_ms + (25LL * 3600 * 1000);
    HU_ASSERT_EQ(hu_daemon_tick_tom_expectation_gc(db, now3, &last_run, interval_sec, ttl_ms),
                 HU_OK);
    HU_ASSERT_EQ(last_run, now3);

    close_db(db);
}

/* ── Phase B (AC-TOM-4): conversation-local belief temporality. ───── */

static sqlite3 *open_in_memory_db_with_beliefs(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_tom_user_beliefs_init_table(db), HU_OK);
    return db;
}

static void test_tom_belief_session_key_separates_across_batches(void) {
    sqlite3 *db = open_in_memory_db_with_beliefs();

    /* Same (contact, topic), two different session_keys -> two rows. */
    HU_ASSERT_EQ(hu_tom_persist_belief(db, "contact_h", "favorite_team", 13, HU_BELIEF_KNOWS, 0.8f,
                                       "session_alpha", 13, /*turn*/ 1, /*now_ms*/ 1000),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_persist_belief(db, "contact_h", "favorite_team", 13, HU_BELIEF_KNOWS, 0.8f,
                                       "session_beta", 12, /*turn*/ 1, /*now_ms*/ 2000),
                 HU_OK);

    int64_t cnt_alpha = 0;
    HU_ASSERT_EQ(
        hu_tom_user_beliefs_count_for_contact_session(db, "contact_h", "session_alpha", &cnt_alpha),
        HU_OK);
    HU_ASSERT_EQ((int)cnt_alpha, 1);

    int64_t cnt_beta = 0;
    HU_ASSERT_EQ(
        hu_tom_user_beliefs_count_for_contact_session(db, "contact_h", "session_beta", &cnt_beta),
        HU_OK);
    HU_ASSERT_EQ((int)cnt_beta, 1);

    /* Same (contact, topic, session) again — UPDATE, not INSERT. */
    HU_ASSERT_EQ(hu_tom_persist_belief(db, "contact_h", "favorite_team", 13, HU_BELIEF_ASSUMES,
                                       0.5f, "session_alpha", 13, 2, 3000),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_tom_user_beliefs_count_for_contact_session(db, "contact_h", "session_alpha", &cnt_alpha),
        HU_OK);
    HU_ASSERT_EQ((int)cnt_alpha, 1);

    close_db(db);
}

static void test_tom_belief_null_session_key_backward_compatible(void) {
    sqlite3 *db = open_in_memory_db_with_beliefs();

    /* Two rows with NULL session_key for the SAME (contact, topic) are
     * allowed because SQLite treats NULL values in UNIQUE indexes as
     * distinct. This is the "pre-temporality / global" backward-compat
     * semantic from AC-TOM-4. */
    HU_ASSERT_EQ(hu_tom_persist_belief(db, "contact_i", "lives_in", 8, HU_BELIEF_KNOWS, 0.7f,
                                       /* session_key */ NULL, 0, /*turn*/ 0, 1000),
                 HU_OK);
    int64_t cnt_null = 0;
    HU_ASSERT_EQ(hu_tom_user_beliefs_count_for_contact_session(db, "contact_i", NULL, &cnt_null),
                 HU_OK);
    HU_ASSERT_EQ((int)cnt_null, 1);

    /* Add a same-(contact, topic) row WITH session_key — distinct row. */
    HU_ASSERT_EQ(hu_tom_persist_belief(db, "contact_i", "lives_in", 8, HU_BELIEF_KNOWS, 0.9f,
                                       "session_x", 9, 1, 2000),
                 HU_OK);
    int64_t cnt_x = 0;
    HU_ASSERT_EQ(
        hu_tom_user_beliefs_count_for_contact_session(db, "contact_i", "session_x", &cnt_x), HU_OK);
    HU_ASSERT_EQ((int)cnt_x, 1);

    /* The NULL-session_key row is preserved and still queryable. */
    HU_ASSERT_EQ(hu_tom_user_beliefs_count_for_contact_session(db, "contact_i", NULL, &cnt_null),
                 HU_OK);
    HU_ASSERT_EQ((int)cnt_null, 1);

    /* Updating the NULL-session_key row in place is also supported — same
     * call with NULL session_key UPDATEs rather than inserting. */
    HU_ASSERT_EQ(hu_tom_persist_belief(db, "contact_i", "lives_in", 8, HU_BELIEF_ASSUMES, 0.4f,
                                       NULL, 0, 0, 3000),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_user_beliefs_count_for_contact_session(db, "contact_i", NULL, &cnt_null),
                 HU_OK);
    HU_ASSERT_EQ((int)cnt_null, 1);

    close_db(db);
}

/* ── Phase C (AC-TOM-5): self-change event recording. ─────────────── */

static sqlite3 *open_in_memory_db_with_self_change(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_tom_self_change_events_init_table(db), HU_OK);
    return db;
}

static void test_tom_self_change_records_persona_delta(void) {
    sqlite3 *db = open_in_memory_db_with_self_change();

    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_j", HU_TOM_SELF_CHANGE_PERSONA_DELTA,
                                                 /*session*/ NULL, 0, /*turn*/ 0,
                                                 /*magnitude*/ 0.85, /*now*/ 1000),
                 HU_OK);
    int64_t cnt = 0;
    HU_ASSERT_EQ(
        hu_tom_self_change_events_count(db, "contact_j", HU_TOM_SELF_CHANGE_PERSONA_DELTA, &cnt),
        HU_OK);
    HU_ASSERT_EQ((int)cnt, 1);
    /* Other event kinds for this contact are 0. */
    HU_ASSERT_EQ(
        hu_tom_self_change_events_count(db, "contact_j", HU_TOM_SELF_CHANGE_ADAPTER_SWAP, &cnt),
        HU_OK);
    HU_ASSERT_EQ((int)cnt, 0);
    close_db(db);
}

static void test_tom_self_change_records_adapter_swap_on_success(void) {
    sqlite3 *db = open_in_memory_db_with_self_change();

    /* Two successful swaps for the same contact -> two rows (events are
     * append-only history, not idempotent). */
    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_k", HU_TOM_SELF_CHANGE_ADAPTER_SWAP,
                                                 NULL, 0, 0, 1.0, 1000),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_k", HU_TOM_SELF_CHANGE_ADAPTER_SWAP,
                                                 NULL, 0, 0, 1.0, 2000),
                 HU_OK);
    int64_t cnt = 0;
    HU_ASSERT_EQ(
        hu_tom_self_change_events_count(db, "contact_k", HU_TOM_SELF_CHANGE_ADAPTER_SWAP, &cnt),
        HU_OK);
    HU_ASSERT_EQ((int)cnt, 2);
    close_db(db);
}

static void test_tom_self_change_records_register_shift(void) {
    sqlite3 *db = open_in_memory_db_with_self_change();

    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_l", HU_TOM_SELF_CHANGE_REGISTER_SHIFT,
                                                 NULL, 0, 0,
                                                 /*sigma*/ 2.3, 1000),
                 HU_OK);
    int64_t cnt = 0;
    HU_ASSERT_EQ(
        hu_tom_self_change_events_count(db, "contact_l", HU_TOM_SELF_CHANGE_REGISTER_SHIFT, &cnt),
        HU_OK);
    HU_ASSERT_EQ((int)cnt, 1);
    close_db(db);
}

static void test_tom_self_change_events_indexed_by_contact_and_timestamp(void) {
    sqlite3 *db = open_in_memory_db_with_self_change();

    /* Three events across two contacts. */
    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_m", HU_TOM_SELF_CHANGE_PERSONA_DELTA,
                                                 NULL, 0, 0, 1.0, 1000),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_m", HU_TOM_SELF_CHANGE_ADAPTER_SWAP,
                                                 NULL, 0, 0, 1.0, 2000),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_n", HU_TOM_SELF_CHANGE_REGISTER_SHIFT,
                                                 NULL, 0, 0, 1.5, 3000),
                 HU_OK);

    /* Count all kinds for contact_m. */
    int64_t cnt = 0;
    HU_ASSERT_EQ(
        hu_tom_self_change_events_count(db, "contact_m", (hu_tom_self_change_kind_t)0, &cnt),
        HU_OK);
    HU_ASSERT_EQ((int)cnt, 2);

    /* contact_n has only the register-shift. */
    HU_ASSERT_EQ(
        hu_tom_self_change_events_count(db, "contact_n", (hu_tom_self_change_kind_t)0, &cnt),
        HU_OK);
    HU_ASSERT_EQ((int)cnt, 1);

    /* Index exists — verify via EXPLAIN that the contact+ts query uses it. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "EXPLAIN QUERY PLAN SELECT * FROM tom_self_change_events "
                                "WHERE contact_id = ? ORDER BY timestamp_utc_ms DESC",
                                -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "contact_m", 9, SQLITE_STATIC);
    bool index_used = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *detail = sqlite3_column_text(stmt, 3);
        if (detail && strstr((const char *)detail, "idx_tom_self_change_contact_ts"))
            index_used = true;
    }
    sqlite3_finalize(stmt);
    HU_ASSERT_TRUE(index_used);

    close_db(db);
}

/* ── Phase D (AC-TOM-6): staleness gap detection. ─────────────────── */

static sqlite3 *open_in_memory_db_for_staleness(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_tom_user_beliefs_init_table(db), HU_OK);
    HU_ASSERT_EQ(hu_tom_self_change_events_init_table(db), HU_OK);
    return db;
}

static void test_tom_staleness_gap_fires_when_persona_delta_pre_dates_belief(void) {
    sqlite3 *db = open_in_memory_db_for_staleness();
    hu_allocator_t alloc = hu_system_allocator();

    /* T1: belief recorded at 1000 ms. T2: persona delta applied at
     * 2000 ms. T3 (now): 3000 ms. Window: 7 days = plenty. */
    const int64_t t1 = 1000;
    const int64_t t2 = 2000;
    const int64_t t3 = 3000;

    HU_ASSERT_EQ(hu_tom_persist_belief(db, "contact_p", "favorite_color", 14, HU_BELIEF_KNOWS, 0.8f,
                                       NULL, 0, 0, t1),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_p", HU_TOM_SELF_CHANGE_PERSONA_DELTA,
                                                 NULL, 0, 0, 1.0, t2),
                 HU_OK);

    hu_tom_staleness_gap_t *gaps = NULL;
    size_t gap_count = 0;
    HU_ASSERT_EQ(hu_tom_detect_staleness_gaps(db, &alloc, "contact_p", t3,
                                              HU_TOM_DEFAULT_STALENESS_WINDOW_SEC, 32, &gaps,
                                              &gap_count),
                 HU_OK);
    HU_ASSERT_EQ((int)gap_count, 1);
    HU_ASSERT_NOT_NULL(gaps);
    HU_ASSERT_STR_EQ(gaps[0].topic, "favorite_color");
    HU_ASSERT_EQ((int)gaps[0].expected_kind, (int)HU_TOM_EXPECT_REMEMBERS);
    HU_ASSERT_EQ((int)gaps[0].event_kind, (int)HU_TOM_SELF_CHANGE_PERSONA_DELTA);
    HU_ASSERT_EQ(gaps[0].belief_ts_ms, t1);
    HU_ASSERT_EQ(gaps[0].event_ts_ms, t2);

    hu_tom_staleness_gaps_free(&alloc, gaps, gap_count);
    close_db(db);
}

static void test_tom_staleness_gap_respects_window(void) {
    sqlite3 *db = open_in_memory_db_for_staleness();
    hu_allocator_t alloc = hu_system_allocator();

    /* Belief at very-old timestamp. Event ALSO very old — outside the
     * window. Expect NO gap. */
    const int64_t old_belief = 1000;
    const int64_t old_event = 2000;
    /* "Now" is 100 days later; window = 7 days. */
    const int64_t now_ms = old_event + ((int64_t)100 * 24 * 60 * 60 * 1000);

    HU_ASSERT_EQ(hu_tom_persist_belief(db, "contact_q", "weather_pref", 12, HU_BELIEF_KNOWS, 0.8f,
                                       NULL, 0, 0, old_belief),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_q", HU_TOM_SELF_CHANGE_PERSONA_DELTA,
                                                 NULL, 0, 0, 1.0, old_event),
                 HU_OK);

    hu_tom_staleness_gap_t *gaps = NULL;
    size_t gap_count = 0;
    HU_ASSERT_EQ(hu_tom_detect_staleness_gaps(db, &alloc, "contact_q", now_ms,
                                              HU_TOM_DEFAULT_STALENESS_WINDOW_SEC, 32, &gaps,
                                              &gap_count),
                 HU_OK);
    HU_ASSERT_EQ((int)gap_count, 0);
    HU_ASSERT_NULL(gaps);

    /* If we relax the window to 200 days, the gap DOES fire — proving
     * the window bound is what's gating, not a bug elsewhere. */
    HU_ASSERT_EQ(hu_tom_detect_staleness_gaps(db, &alloc, "contact_q", now_ms,
                                              /*window_sec*/ (int64_t)200 * 24 * 60 * 60, 32, &gaps,
                                              &gap_count),
                 HU_OK);
    HU_ASSERT_EQ((int)gap_count, 1);
    hu_tom_staleness_gaps_free(&alloc, gaps, gap_count);

    close_db(db);
}

static void test_tom_staleness_gap_kind_mapping(void) {
    /* AC-TOM-6 / D-TOM-6: PERSONA_DELTA maps to REMEMBERS,
     * ADAPTER_SWAP to UNDERSTANDS, REGISTER_SHIFT to TRACKS. The mapping
     * is exposed as a pure predicate. */
    HU_ASSERT_EQ((int)hu_tom_self_change_invalidates_kind(HU_TOM_SELF_CHANGE_PERSONA_DELTA),
                 (int)HU_TOM_EXPECT_REMEMBERS);
    HU_ASSERT_EQ((int)hu_tom_self_change_invalidates_kind(HU_TOM_SELF_CHANGE_ADAPTER_SWAP),
                 (int)HU_TOM_EXPECT_UNDERSTANDS);
    HU_ASSERT_EQ((int)hu_tom_self_change_invalidates_kind(HU_TOM_SELF_CHANGE_REGISTER_SHIFT),
                 (int)HU_TOM_EXPECT_TRACKS);

    /* Now a mixed-event scenario: a belief + three events of distinct
     * kinds, all in-window. Expect THREE gaps, each carrying the
     * correctly-mapped expected_kind. */
    sqlite3 *db = open_in_memory_db_for_staleness();
    hu_allocator_t alloc = hu_system_allocator();
    const int64_t t_belief = 1000;
    const int64_t t_event_a = 2000;
    const int64_t t_event_b = 3000;
    const int64_t t_event_c = 4000;
    const int64_t now_ms = 5000;

    HU_ASSERT_EQ(hu_tom_persist_belief(db, "contact_r", "preferred_pace", 14, HU_BELIEF_KNOWS, 0.8f,
                                       NULL, 0, 0, t_belief),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_r", HU_TOM_SELF_CHANGE_PERSONA_DELTA,
                                                 NULL, 0, 0, 1.0, t_event_a),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_r", HU_TOM_SELF_CHANGE_ADAPTER_SWAP,
                                                 NULL, 0, 0, 1.0, t_event_b),
                 HU_OK);
    HU_ASSERT_EQ(hu_tom_record_self_change_event(db, "contact_r", HU_TOM_SELF_CHANGE_REGISTER_SHIFT,
                                                 NULL, 0, 0, 1.5, t_event_c),
                 HU_OK);

    hu_tom_staleness_gap_t *gaps = NULL;
    size_t gap_count = 0;
    HU_ASSERT_EQ(hu_tom_detect_staleness_gaps(db, &alloc, "contact_r", now_ms,
                                              HU_TOM_DEFAULT_STALENESS_WINDOW_SEC, 32, &gaps,
                                              &gap_count),
                 HU_OK);
    HU_ASSERT_EQ((int)gap_count, 3);

    /* Tally the kinds we observe (order not asserted because the SQL
     * ORDER BY uses event timestamp DESC; the assertion here is
     * "each kind appears exactly once with the correct mapping"). */
    int seen_remembers = 0, seen_understands = 0, seen_tracks = 0;
    for (size_t i = 0; i < gap_count; i++) {
        if (gaps[i].event_kind == HU_TOM_SELF_CHANGE_PERSONA_DELTA) {
            HU_ASSERT_EQ((int)gaps[i].expected_kind, (int)HU_TOM_EXPECT_REMEMBERS);
            seen_remembers++;
        } else if (gaps[i].event_kind == HU_TOM_SELF_CHANGE_ADAPTER_SWAP) {
            HU_ASSERT_EQ((int)gaps[i].expected_kind, (int)HU_TOM_EXPECT_UNDERSTANDS);
            seen_understands++;
        } else if (gaps[i].event_kind == HU_TOM_SELF_CHANGE_REGISTER_SHIFT) {
            HU_ASSERT_EQ((int)gaps[i].expected_kind, (int)HU_TOM_EXPECT_TRACKS);
            seen_tracks++;
        }
    }
    HU_ASSERT_EQ(seen_remembers, 1);
    HU_ASSERT_EQ(seen_understands, 1);
    HU_ASSERT_EQ(seen_tracks, 1);

    hu_tom_staleness_gaps_free(&alloc, gaps, gap_count);
    close_db(db);
}

void run_tom_activation_tests(void) {
    HU_TEST_SUITE("tom_activation");
    HU_RUN_TEST(test_tom_detect_user_expectation_fires_on_as_you_know_phrase);
    HU_RUN_TEST(test_tom_record_user_expectation_idempotent_on_unique_tuple);
    HU_RUN_TEST(test_tom_build_context_includes_unmet_expectations_section);
    HU_RUN_TEST(test_tom_build_context_omits_section_when_no_unmet);
    HU_RUN_TEST(test_tom_expectation_gc_deletes_old_resolved);
    HU_RUN_TEST(test_tom_expectation_gc_preserves_recent_or_unresolved);
    HU_RUN_TEST(test_tom_expectation_gc_tick_respects_interval);
    /* Phase B (AC-TOM-4): belief temporality. */
    HU_RUN_TEST(test_tom_belief_session_key_separates_across_batches);
    HU_RUN_TEST(test_tom_belief_null_session_key_backward_compatible);
    /* Phase C (AC-TOM-5): self-change event recording. */
    HU_RUN_TEST(test_tom_self_change_records_persona_delta);
    HU_RUN_TEST(test_tom_self_change_records_adapter_swap_on_success);
    HU_RUN_TEST(test_tom_self_change_records_register_shift);
    HU_RUN_TEST(test_tom_self_change_events_indexed_by_contact_and_timestamp);
    /* Phase D (AC-TOM-6): staleness gap detection. */
    HU_RUN_TEST(test_tom_staleness_gap_fires_when_persona_delta_pre_dates_belief);
    HU_RUN_TEST(test_tom_staleness_gap_respects_window);
    HU_RUN_TEST(test_tom_staleness_gap_kind_mapping);
}

#else /* HU_ENABLE_SQLITE */

void run_tom_activation_tests(void) {
    /* Phase A persisted-TOM code is gated on HU_ENABLE_SQLITE; on minimal
     * / no-sqlite builds the surface is omitted entirely. Compile a stub
     * runner so the symbol resolves (per test-source-gate-symmetry rule
     * pattern 2: internal-#ifdef-wrap-with-stub-runner). */
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
