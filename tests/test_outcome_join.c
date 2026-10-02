/* tests/test_outcome_join.c — DEF-8: a contact's tapback on a reply the DAEMON
 * delivered must land in production_outcomes.tapback_polarity (and the DPO
 * pair store); a tapback on a message SETH typed must not.
 *
 * Production facts this pins (2026-10-02, aggregate counts):
 *  - 1,695 of 1,725 of our sent chat.db rows (30 d) have text NULL, so the
 *    registration-time GUID lookup missed and the router registered a
 *    synthetic "out-<ts>" msg_ref (440 of 492 reaction_lookup rows); a
 *    tapback carries the real GUID, so the exact join never hit.
 *  - 652 of 661 production_outcomes rows were already resolved by the
 *    contact's reply, and hu_dpo_record_outcome only updates unresolved rows.
 *  - is_from_me=1 is also true of Seth's own typing, so the join must be
 *    anchored on an outbound_sends row (the daemon's own delivery record). */
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/contact_send_recency.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory/outbound_sends_repo.h"
#include "human/ml/dpo.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char k_contact[] = "+15550001111";
#define OJ_BOUNDARY  100 /* chat.db max ROWID before the daemon's send */
#define OJ_REPLY_ROW 101

typedef struct {
    sqlite3 *db;
    hu_dpo_collector_t col;
    int64_t sent_ms; /* when the daemon's reply was delivered */
} oj_fixture_t;

static int s_sink_calls;
static int64_t s_sink_ms;
static void oj_sink(const char *contact, size_t len, int64_t delivered_ms) {
    (void)contact;
    (void)len;
    s_sink_calls++;
    s_sink_ms = delivered_ms;
}

/* Production shape: the daemon delivers a reply (outbound_sends row with its
 * chat.db boundary, production_outcomes row, reaction_lookup registration
 * under a synthetic "out-<ts>" ref because the GUID lookup missed), then the
 * contact's text reply resolves the outcome row. */
static void oj_setup(oj_fixture_t *f) {
    hu_allocator_t alloc = hu_system_allocator();
    f->db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &f->db), SQLITE_OK);
    memset(&f->col, 0, sizeof(f->col));
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, f->db, 1024, &f->col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&f->col), HU_OK);
    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&f->col);
    s_sink_calls = 0;
    s_sink_ms = 0;
    hu_reaction_handler_set_engagement_sink(oj_sink);

    f->sent_ms = (int64_t)time(NULL) * 1000;
    HU_ASSERT_EQ(hu_outbound_sends_repo_record(f->db, f->sent_ms, "imessage", k_contact,
                                               strlen(k_contact), HU_OUTBOUND_SEND_KIND_TEXT,
                                               "yeah after 6", 12, OJ_BOUNDARY),
                 HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_outbound(&f->col, "imessage", 8, k_contact, strlen(k_contact), NULL,
                                        0, "you around later?", 17, "yeah after 6", 12, 0.5, NULL,
                                        0),
                 HU_OK);
    char ref[32];
    snprintf(ref, sizeof(ref), "out-%lld", (long long)time(NULL));
    hu_reaction_handler_register_assistant_message_for_test(
        "imessage", k_contact, ref, "you around later?", "yeah after 6", "");
    HU_ASSERT_EQ(
        hu_dpo_record_inbound_arrival(&f->col, "imessage", 8, k_contact, strlen(k_contact), 9),
        HU_OK);
}

static void oj_teardown(oj_fixture_t *f) {
    hu_reaction_handler_set_outcome_join_mode_for_test(-1);
    hu_reaction_handler_set_engagement_sink(NULL);
    hu_reaction_handler_reset_for_test();
    hu_dpo_collector_deinit(&f->col);
    sqlite3_close(f->db);
}

/* 2 = NULL column, 9 = no row, else the stored polarity. */
static int oj_tapback_polarity(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT tapback_polarity FROM production_outcomes LIMIT 1", -1, &st,
                           NULL) != SQLITE_OK)
        return 9;
    int v = 9;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_type(st, 0) == SQLITE_NULL ? 2 : sqlite3_column_int(st, 0);
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

static size_t oj_pairs(oj_fixture_t *f) {
    size_t n = 99;
    HU_ASSERT_EQ(hu_dpo_pair_count(&f->col, &n), HU_OK);
    return n;
}

/* The contact loves the reply the daemon delivered (chat.db row 101, first
 * is_from_me row after the send's boundary, stamped 200 ms after delivery). */
static hu_reaction_event_t oj_tap_on_daemon_reply(const oj_fixture_t *f) {
    hu_reaction_event_t e = {
        .channel_id = "imessage",
        .target_thread_id = k_contact,
        .target_message_ref = "4C1D2E3F-0000-4000-8000-00000000BEEF", /* real chat.db GUID */
        .sender_handle = k_contact,
        .kind = HU_REACTION_LOVE,
        .polarity = HU_REACTION_POSITIVE,
        .timestamp_unix = (int64_t)time(NULL) + 30,
        .target_is_ours = 1,
        .target_rowid = OJ_REPLY_ROW,
        .target_prev_own_rowid = 90,
        .target_sent_ms = f->sent_ms + 200,
    };
    return e;
}

/* Headline: LIVE lands the tapback on the row the reply already resolved,
 * yields the DPO pair the exact join never could, and credits engagement. */
static void outcome_join_live_lands_tapback_on_daemon_reply(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_LIVE);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), 2); /* pre: NULL, as in prod */
    HU_ASSERT_TRUE(oj_reply_latency_present(f.db));
    HU_ASSERT_EQ(oj_pairs(&f), 0);

    hu_reaction_event_t e = oj_tap_on_daemon_reply(&f);
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    HU_ASSERT_EQ(oj_tapback_polarity(f.db), 1);
    HU_ASSERT_TRUE(oj_reply_latency_present(f.db)); /* reply signal untouched */
    HU_ASSERT_EQ(oj_pairs(&f), 1);
    HU_ASSERT_EQ(s_sink_calls, 1);
    HU_ASSERT_EQ(s_sink_ms, f.sent_ms);
    oj_teardown(&f);
}

/* CRITICAL: Seth types his own message five minutes after the daemon's reply
 * and the contact taps it. is_from_me=1, so target_is_ours is set — but no
 * daemon send claims that row, so nothing is credited to the daemon. */
static void outcome_join_seth_typed_message_not_credited(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_LIVE);
    hu_reaction_event_t e = oj_tap_on_daemon_reply(&f);
    e.target_rowid = 140;
    e.target_prev_own_rowid = OJ_REPLY_ROW; /* the daemon's reply came before it */
    e.target_sent_ms = f.sent_ms + 5 * 60 * 1000;
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), 2);
    HU_ASSERT_EQ(oj_pairs(&f), 0);
    HU_ASSERT_EQ(s_sink_calls, 0);
    oj_teardown(&f);
}

/* Even 2 s after the daemon's reply — inside any time window — Seth's own row
 * is not the daemon's: the send's boundary sits before our previous message. */
static void outcome_join_seth_typed_right_after_not_credited(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_LIVE);
    hu_reaction_event_t e = oj_tap_on_daemon_reply(&f);
    e.target_rowid = OJ_REPLY_ROW + 1;
    e.target_prev_own_rowid = OJ_REPLY_ROW;
    e.target_sent_ms = f.sent_ms + 2000;
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), 2);
    HU_ASSERT_EQ(oj_pairs(&f), 0);
    oj_teardown(&f);
}

/* love -> dislike on the same reply: the polarity is replaced and ONE pair
 * remains (rejected = the reply), not two opposite ones. */
static void outcome_join_changed_reaction_replaces(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_LIVE);
    hu_reaction_event_t e = oj_tap_on_daemon_reply(&f);
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);
    HU_ASSERT_EQ(oj_pairs(&f), 1);
    e.kind = HU_REACTION_DISLIKE;
    e.polarity = HU_REACTION_NEGATIVE;
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), -1);
    HU_ASSERT_EQ(oj_pairs(&f), 1);
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(f.db, "SELECT rejected FROM dpo_pairs LIMIT 1", -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 0), "yeah after 6");
    sqlite3_finalize(st);
    oj_teardown(&f);
}

/* A 😢 custom-emoji tapback is negative in LIVE, not the code's blanket +1. */
static void outcome_join_custom_emoji_polarity_from_glyph(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_LIVE);
    hu_reaction_event_t e = oj_tap_on_daemon_reply(&f);
    e.kind = HU_REACTION_KIND_CUSTOM_EMOJI;
    e.polarity = HU_REACTION_POSITIVE; /* what the 2006 code maps to */
    e.emoji = "\U0001F622";
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), -1);
    oj_teardown(&f);
}

static void reaction_emoji_polarity_mapping(void) {
    HU_ASSERT_EQ(hu_reaction_emoji_polarity("\U0001F622"), HU_REACTION_NEGATIVE);
    HU_ASSERT_EQ(hu_reaction_emoji_polarity("\U0001F44E"), HU_REACTION_NEGATIVE);
    HU_ASSERT_EQ(hu_reaction_emoji_polarity("❤️"), HU_REACTION_POSITIVE);
    HU_ASSERT_EQ(hu_reaction_emoji_polarity("\U0001F602"), HU_REACTION_POSITIVE);
    HU_ASSERT_EQ(hu_reaction_emoji_polarity("\U0001F9C0"), HU_REACTION_NEUTRAL); /* cheese */
    HU_ASSERT_EQ(hu_reaction_emoji_polarity(NULL), HU_REACTION_NEUTRAL);
}

/* OFF is byte-identical to before: NOT_FOUND, nothing written, no sink. */
static void outcome_join_off_unchanged(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_OFF);
    hu_reaction_event_t e = oj_tap_on_daemon_reply(&f);
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), 2);
    HU_ASSERT_EQ(oj_pairs(&f), 0);
    HU_ASSERT_EQ(s_sink_calls, 0);
    oj_teardown(&f);
}

/* SHADOW finds the join but writes nothing to outcomes or pairs. */
static void outcome_join_shadow_writes_nothing(void) {
    oj_fixture_t f;
    oj_setup(&f);
    hu_reaction_handler_set_outcome_join_mode_for_test(HU_GATE_SHADOW);
    hu_reaction_event_t e = oj_tap_on_daemon_reply(&f);
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(oj_tapback_polarity(f.db), 2);
    HU_ASSERT_EQ(oj_pairs(&f), 0);
    oj_teardown(&f);
}

void run_outcome_join_tests(void) {
    HU_TEST_SUITE("outcome_join");
    HU_RUN_TEST(outcome_join_live_lands_tapback_on_daemon_reply);
    HU_RUN_TEST(outcome_join_seth_typed_message_not_credited);
    HU_RUN_TEST(outcome_join_seth_typed_right_after_not_credited);
    HU_RUN_TEST(outcome_join_changed_reaction_replaces);
    HU_RUN_TEST(outcome_join_custom_emoji_polarity_from_glyph);
    HU_RUN_TEST(reaction_emoji_polarity_mapping);
    HU_RUN_TEST(outcome_join_off_unchanged);
    HU_RUN_TEST(outcome_join_shadow_writes_nothing);
}
