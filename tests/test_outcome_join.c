/* tests/test_outcome_join.c — DEF-8: a contact's tapback on OUR reply must
 * land in production_outcomes.tapback_polarity (and the DPO pair store).
 *
 * Production facts this pins (2026-10-02, aggregate counts):
 *  - 1,695 of 1,725 of our sent chat.db rows (30 d) have text NULL (body in
 *    attributedBody), so the registration-time GUID lookup
 *    (`m.text LIKE ?2 || '%'`) missed and the router registered a synthetic
 *    "out-<ts>" msg_ref: 440 of 492 reaction_lookup rows. A tapback carries
 *    the real GUID, so the exact join could never hit.
 *  - 652 of 661 production_outcomes rows were already resolved by the
 *    contact's reply (reply_latency_s), and hu_dpo_record_outcome only
 *    updates rows with outcome_resolved_at IS NULL — so even a hit could
 *    not land.
 * The fix joins by (thread, target send time) under HU_OUTCOME_JOIN. */
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/ml/dpo.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    sqlite3 *db;
    hu_dpo_collector_t col;
} oj_fixture_t;

/* Production shape: the reply is recorded (record_outbound, message_ref NULL),
 * the router registers it under a synthetic "out-<ts>" ref because the GUID
 * lookup missed, then the contact's text reply resolves the row. */
static void oj_setup(oj_fixture_t *f) {
    hu_allocator_t alloc = hu_system_allocator();
    f->db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &f->db), SQLITE_OK);
    memset(&f->col, 0, sizeof(f->col));
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, f->db, 1024, &f->col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&f->col), HU_OK);
    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&f->col);

    HU_ASSERT_EQ(hu_dpo_record_outbound(&f->col, "imessage", 8, "+15550001111", 12, NULL, 0,
                                        "you around later?", 17, "yeah after 6", 12, 0.5, NULL, 0),
                 HU_OK);
    char ref[32];
    snprintf(ref, sizeof(ref), "out-%lld", (long long)time(NULL));
    hu_reaction_handler_register_assistant_message_for_test(
        "imessage", "+15550001111", ref, "you around later?", "yeah after 6", "");
    /* Contact replies by text first: resolves the row (latency + length). */
    HU_ASSERT_EQ(hu_dpo_record_inbound_arrival(&f->col, "imessage", 8, "+15550001111", 12, 9),
                 HU_OK);
}

static void oj_teardown(oj_fixture_t *f) {
    hu_reaction_handler_set_outcome_join_mode_for_test(-1);
    hu_reaction_handler_reset_for_test();
    hu_dpo_collector_deinit(&f->col);
    sqlite3_close(f->db);
}

/* -1 = NULL column, -9 = no row. */
static int oj_tapback_polarity(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT tapback_polarity FROM production_outcomes LIMIT 1", -1, &st,
                           NULL) != SQLITE_OK)
        return -9;
    int v = -9;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_type(st, 0) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

static int oj_reply_latency_present(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT reply_latency_s FROM production_outcomes LIMIT 1", -1, &st,
                           NULL) != SQLITE_OK)
        return 0;
    int v = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_type(st, 0) != SQLITE_NULL;
    sqlite3_finalize(st);
    return v;
}

static hu_reaction_event_t oj_tapback_on_our_reply(void) {
    hu_reaction_event_t e = {
        .channel_id = "imessage",
        .target_thread_id = "+15550001111",
        .target_message_ref = "4C1D2E3F-0000-4000-8000-00000000BEEF", /* real chat.db GUID */
        .sender_handle = "+15550001111",
        .kind = HU_REACTION_LOVE,
        .polarity = HU_REACTION_POSITIVE,
        .timestamp_unix = (int64_t)time(NULL) + 30,
        .target_is_ours = 1,
        .target_sent_unix = (int64_t)time(NULL) + 2,
    };
    return e;
}

/* Headline: LIVE lands the tapback on the row the reply already resolved,
 * and yields the DPO pair the exact join never could. */
static void outcome_join_live_lands_tapback_on_resolved_row(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_LIVE);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), -1); /* pre: NULL, as in prod */
    HU_ASSERT_TRUE(oj_reply_latency_present(f.db));
    size_t pairs_before = 99;
    HU_ASSERT_EQ(hu_dpo_pair_count(&f.col, &pairs_before), HU_OK);
    HU_ASSERT_EQ(pairs_before, 0);

    hu_reaction_event_t e = oj_tapback_on_our_reply();
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    HU_ASSERT_EQ(oj_tapback_polarity(f.db), 1);
    HU_ASSERT_TRUE(oj_reply_latency_present(f.db)); /* reply signal untouched */
    size_t pairs_after = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&f.col, &pairs_after), HU_OK);
    HU_ASSERT_EQ(pairs_after, 1);
    oj_teardown(&f);
}

/* Negative tapback lands as -1. */
static void outcome_join_live_negative_tapback(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_LIVE);
    hu_reaction_event_t e = oj_tapback_on_our_reply();
    e.kind = HU_REACTION_DISLIKE;
    e.polarity = HU_REACTION_NEGATIVE;
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);
    /* -1 is ALSO the helper's NULL sentinel, so read the raw column. */
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(f.db,
                                    "SELECT tapback_polarity IS NOT NULL, tapback_polarity "
                                    "FROM production_outcomes LIMIT 1",
                                    -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 1);
    HU_ASSERT_EQ(sqlite3_column_int(st, 1), -1);
    sqlite3_finalize(st);
    oj_teardown(&f);
}

/* A tapback on a message that is NOT ours never joins, even when LIVE. */
static void outcome_join_live_ignores_tapback_not_on_ours(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_LIVE);
    hu_reaction_event_t e = oj_tapback_on_our_reply();
    e.target_is_ours = 0;
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), -1);
    size_t n = 9;
    HU_ASSERT_EQ(hu_dpo_pair_count(&f.col, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    oj_teardown(&f);
}

/* Our message from a time with no recorded reply (Seth typed it, or an old
 * one outside the window) never borrows a later row. */
static void outcome_join_live_ignores_send_time_outside_window(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_LIVE);
    hu_reaction_event_t e = oj_tapback_on_our_reply();
    e.target_sent_unix = (int64_t)time(NULL) - 3 * 86400;
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), -1);
    oj_teardown(&f);
}

/* OFF is byte-identical to before: NOT_FOUND, nothing written. */
static void outcome_join_off_unchanged(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_OFF);
    hu_reaction_event_t e = oj_tapback_on_our_reply();
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), -1);
    size_t n = 9;
    HU_ASSERT_EQ(hu_dpo_pair_count(&f.col, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    oj_teardown(&f);
}

/* SHADOW computes the join but writes nothing. */
static void outcome_join_shadow_writes_nothing(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_SHADOW);
    hu_reaction_event_t e = oj_tapback_on_our_reply();
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), -1);
    size_t n = 9;
    HU_ASSERT_EQ(hu_dpo_pair_count(&f.col, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    oj_teardown(&f);
}

void run_outcome_join_tests(void) {
    HU_TEST_SUITE("outcome_join");
    HU_RUN_TEST(outcome_join_live_lands_tapback_on_resolved_row);
    HU_RUN_TEST(outcome_join_live_negative_tapback);
    HU_RUN_TEST(outcome_join_live_ignores_tapback_not_on_ours);
    HU_RUN_TEST(outcome_join_live_ignores_send_time_outside_window);
    HU_RUN_TEST(outcome_join_off_unchanged);
    HU_RUN_TEST(outcome_join_shadow_writes_nothing);
}
