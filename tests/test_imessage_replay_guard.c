/* Replay guards for the iMessage ingest path.
 *
 * Incident 2026-09-01: after a reboot the daemon resumed from a two-week-stale
 * persisted rowid, replayed ~2,000 old inbound messages as if fresh, and sent
 * "sorry just saw this" + replies to real contacts for threads Seth had already
 * closed himself. These tests pin the three guards that prevent a repeat:
 *   1. resume cursor is capped (large gap → seed from db max, not replay);
 *   2. stale inbound rows are dropped at poll time;
 *   3. an inbound row that already has a later human-authored outbound in the
 *      same chat is skipped (the daemon's OWN later sends do not count). */
#include "human/channels/imessage.h"
#include "test_framework.h"
#include <string.h>

/* ── 1. hu_imessage_resume_rowid (pure) ─────────────────────────────── */

static void test_resume_no_persisted_seeds_from_db_max(void) {
    int64_t skipped = -1;
    HU_ASSERT_EQ(hu_imessage_resume_rowid(0, 70491, 50, &skipped), 70491);
    HU_ASSERT_EQ(skipped, 0);
}

static void test_resume_persisted_ahead_of_db_seeds_from_db_max(void) {
    int64_t skipped = -1;
    HU_ASSERT_EQ(hu_imessage_resume_rowid(80000, 70491, 50, &skipped), 70491);
    HU_ASSERT_EQ(skipped, 0);
}

static void test_resume_small_gap_replays_from_persisted(void) {
    int64_t skipped = -1;
    HU_ASSERT_EQ(hu_imessage_resume_rowid(70481, 70491, 50, &skipped), 70481);
    HU_ASSERT_EQ(skipped, 0);
}

static void test_resume_gap_equal_to_cap_still_replays(void) {
    int64_t skipped = -1;
    HU_ASSERT_EQ(hu_imessage_resume_rowid(70441, 70491, 50, &skipped), 70441);
    HU_ASSERT_EQ(skipped, 0);
}

static void test_resume_large_gap_is_capped_to_db_max(void) {
    /* The incident shape: persisted=69593, db max=70486 → 893 behind. */
    int64_t skipped = -1;
    HU_ASSERT_EQ(hu_imessage_resume_rowid(69593, 70486, 50, &skipped), 70486);
    HU_ASSERT_EQ(skipped, 893);
}

static void test_resume_null_out_skipped_is_tolerated(void) {
    HU_ASSERT_EQ(hu_imessage_resume_rowid(69593, 70486, 50, NULL), 70486);
}

/* ── 2. hu_imessage_inbound_is_stale (pure) ─────────────────────────── */

static void test_stale_unknown_timestamp_is_not_stale(void) {
    HU_ASSERT_FALSE(hu_imessage_inbound_is_stale(0, 1788309259, 86400));
}

static void test_stale_recent_message_is_not_stale(void) {
    HU_ASSERT_FALSE(hu_imessage_inbound_is_stale(1788309259 - 3600, 1788309259, 86400));
}

static void test_stale_exactly_max_age_is_not_stale(void) {
    HU_ASSERT_FALSE(hu_imessage_inbound_is_stale(1788309259 - 86400, 1788309259, 86400));
}

static void test_stale_one_past_max_age_is_stale(void) {
    HU_ASSERT_TRUE(hu_imessage_inbound_is_stale(1788309259 - 86401, 1788309259, 86400));
}

static void test_stale_sixteen_day_old_message_is_stale(void) {
    HU_ASSERT_TRUE(hu_imessage_inbound_is_stale(1788309259 - 16LL * 86400, 1788309259, 86400));
}

static void test_stale_zero_max_age_disables_guard(void) {
    HU_ASSERT_FALSE(hu_imessage_inbound_is_stale(1788309259 - 16LL * 86400, 1788309259, 0));
}

static void test_stale_apple_epoch_zero_date_is_unknown_not_stale(void) {
    /* chat.db m.date == 0 → unix_ts == 978307200 (2001-01-01). That is a
     * missing date, not a 25-year-old message: treat as unknown. */
    HU_ASSERT_FALSE(hu_imessage_inbound_is_stale(978307200, 1788309259, 86400));
    HU_ASSERT_FALSE(hu_imessage_inbound_is_stale(978307199, 1788309259, 86400));
}

/* ── 2b. hu_imessage_parse_env_int64 (pure) — garbage must NOT disable a guard ── */

static void test_env_unset_returns_default(void) {
    HU_ASSERT_EQ(hu_imessage_parse_env_int64(NULL, 50), 50);
    HU_ASSERT_EQ(hu_imessage_parse_env_int64("", 50), 50);
}

static void test_env_valid_number_is_used(void) {
    HU_ASSERT_EQ(hu_imessage_parse_env_int64("200", 50), 200);
    HU_ASSERT_EQ(hu_imessage_parse_env_int64("0", 50), 0);
}

static void test_env_garbage_falls_back_to_default(void) {
    HU_ASSERT_EQ(hu_imessage_parse_env_int64("abc", 50), 50);
    HU_ASSERT_EQ(hu_imessage_parse_env_int64("12abc", 50), 50);
    HU_ASSERT_EQ(hu_imessage_parse_env_int64("-5", 50), 50);
    HU_ASSERT_EQ(hu_imessage_parse_env_int64(" 7", 50), 50);
}

/* ── 3a. hu_imessage_replied_guard_applies (pure) — never on the self-chat ── */

static void test_replied_guard_applies_to_normal_handle(void) {
    HU_ASSERT_TRUE(hu_imessage_replied_guard_applies("+15551110001", "+15559990000"));
    HU_ASSERT_TRUE(hu_imessage_replied_guard_applies("+15551110001", NULL));
    HU_ASSERT_TRUE(hu_imessage_replied_guard_applies("+15551110001", ""));
}

static void test_replied_guard_skips_loopback_handle(void) {
    /* Self-chat rows are is_from_me=1 by definition; the next self-typed
     * command would look like "human already replied" and swallow the one
     * before it. Case-insensitive because handles are emails too. */
    HU_ASSERT_FALSE(hu_imessage_replied_guard_applies("+15559990000", "+15559990000"));
    HU_ASSERT_FALSE(hu_imessage_replied_guard_applies("Me@Example.com", "me@example.com"));
}

static void test_replied_guard_null_handle_does_not_apply(void) {
    HU_ASSERT_FALSE(hu_imessage_replied_guard_applies(NULL, "+15559990000"));
}

/* ── 3. hu_imessage_user_replied_after (chat.db fixture) ────────────── */
#if HU_HAS_IMESSAGE && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>

static const char *guard_schema_sql =
    "CREATE TABLE handle (ROWID INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT UNIQUE NOT NULL);"
    "CREATE TABLE message (ROWID INTEGER PRIMARY KEY AUTOINCREMENT, guid TEXT UNIQUE,"
    "  text TEXT, handle_id INTEGER, date INTEGER DEFAULT 0, is_from_me INTEGER DEFAULT 0,"
    "  associated_message_type INTEGER DEFAULT 0, attributedBody BLOB);"
    "CREATE TABLE chat (ROWID INTEGER PRIMARY KEY AUTOINCREMENT, guid TEXT UNIQUE);"
    "CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);";

/* handle 1 = friend A (chat 1), handle 2 = friend B (chat 2).
 *  rowid 1: A inbound "hey"               (chat 1)
 *  rowid 2: outbound to A "on it"         (chat 1)  <- Seth's human reply
 *  rowid 3: A inbound "you there?"        (chat 1)
 *  rowid 4: B inbound "sup"               (chat 2)
 *  rowid 5: outbound tapback to B         (chat 2, associated_message_type=2000)
 *  rowid 6: A inbound "one more"          (chat 1)
 *  rowid 7: outbound to A "auto reply"    (chat 1)  <- the daemon's own send */
static const char *guard_seed_sql =
    "INSERT INTO handle (id) VALUES ('+15551110001');"
    "INSERT INTO handle (id) VALUES ('+15551110002');"
    "INSERT INTO chat (guid) VALUES ('iMessage;-;+15551110001');"
    "INSERT INTO chat (guid) VALUES ('iMessage;-;+15551110002');"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me) VALUES ('G1','hey',1,100,0);"
    "INSERT INTO chat_message_join VALUES (1,1);"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me) VALUES ('G2','on it',1,200,1);"
    "INSERT INTO chat_message_join VALUES (1,2);"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me) VALUES ('G3','you there?',1,300,0);"
    "INSERT INTO chat_message_join VALUES (1,3);"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me) VALUES ('G4','sup',2,400,0);"
    "INSERT INTO chat_message_join VALUES (2,4);"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me,associated_message_type)"
    "  VALUES ('G5',NULL,2,500,1,2000);"
    "INSERT INTO chat_message_join VALUES (2,5);"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me) VALUES ('G6','one more',1,600,0);"
    "INSERT INTO chat_message_join VALUES (1,6);"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me) VALUES ('G7','auto reply',1,700,1);"
    "INSERT INTO chat_message_join VALUES (1,7);"
    /* Group chat 3 (A and B):
     *  rowid 8: B inbound "who's in?"      (chat 3)
     *  rowid 9: Seth outbound "running late" (chat 3) — unrelated to 8 */
    "INSERT INTO chat (guid) VALUES ('iMessage;+;chat123');"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me) VALUES ('G8','who''s in?',2,800,0);"
    "INSERT INTO chat_message_join VALUES (3,8);"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me) VALUES ('G9','running "
    "late',1,900,1);"
    "INSERT INTO chat_message_join VALUES (3,9);"
    /* handle 3 = friend C (chat 4):
     * rowid 10: C inbound "ping"          (chat 4)
     * rowid 11: Seth outbound, text NULL, attributedBody = typedstream "yo" (chat 4) */
    "INSERT INTO handle (id) VALUES ('+15551110003');"
    "INSERT INTO chat (guid) VALUES ('iMessage;-;+15551110003');"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me) VALUES ('G10','ping',3,1000,0);"
    "INSERT INTO chat_message_join VALUES (4,10);"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me,attributedBody)"
    "  VALUES ('G11',NULL,3,1100,1,X'012B02796F');"
    "INSERT INTO chat_message_join VALUES (4,11);"
    /* handle 4 = friend D (chat 5):
     * rowid 12: D inbound "?"             (chat 5)
     * rowid 13: Seth outbound, text NULL, unparseable attributedBody (chat 5) */
    "INSERT INTO handle (id) VALUES ('+15551110004');"
    "INSERT INTO chat (guid) VALUES ('iMessage;-;+15551110004');"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me) VALUES ('G12','?',4,1200,0);"
    "INSERT INTO chat_message_join VALUES (5,12);"
    "INSERT INTO message (guid,text,handle_id,date,is_from_me,attributedBody)"
    "  VALUES ('G13',NULL,4,1300,1,X'0000000000');"
    "INSERT INTO chat_message_join VALUES (5,13);";

static sqlite3 *open_guard_fixture(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK || !db)
        return NULL;
    if (sqlite3_exec(db, guard_schema_sql, NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_exec(db, guard_seed_sql, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static bool never_ours(void *ctx, const char *text, size_t len) {
    (void)ctx;
    (void)text;
    (void)len;
    return false;
}

static bool auto_reply_is_ours(void *ctx, const char *text, size_t len) {
    (void)ctx;
    return len == 10 && strncmp(text, "auto reply", 10) == 0;
}

static bool yo_is_ours(void *ctx, const char *text, size_t len) {
    (void)ctx;
    return len == 2 && strncmp(text, "yo", 2) == 0;
}

static void test_replied_after_human_outbound_in_same_chat(void) {
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_TRUE(hu_imessage_user_replied_after(db, "iMessage;-;+15551110001", "+15551110001", 1,
                                                  never_ours, NULL));
    sqlite3_close(db);
}

static void test_not_replied_when_only_our_send_follows(void) {
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    /* rowid 3 has outbound 7 after it, but 7 is the daemon's own send. */
    HU_ASSERT_FALSE(hu_imessage_user_replied_after(db, "iMessage;-;+15551110001", "+15551110001", 3,
                                                   auto_reply_is_ours, NULL));
    sqlite3_close(db);
}

static void test_daemon_own_later_send_does_not_count(void) {
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_FALSE(hu_imessage_user_replied_after(db, "iMessage;-;+15551110001", "+15551110001", 6,
                                                   auto_reply_is_ours, NULL));
    /* ...but with no echo knowledge (fresh process after restart) it DOES count,
     * which is exactly the replay-after-reboot case we want to skip. */
    HU_ASSERT_TRUE(hu_imessage_user_replied_after(db, "iMessage;-;+15551110001", "+15551110001", 6,
                                                  never_ours, NULL));
    sqlite3_close(db);
}

static void test_outbound_in_other_chat_does_not_count(void) {
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    /* B's inbound (rowid 4): the only later real outbound is in A's chat. */
    HU_ASSERT_FALSE(hu_imessage_user_replied_after(db, "iMessage;-;+15551110002", "+15551110002", 4,
                                                   never_ours, NULL));
    sqlite3_close(db);
}

static void test_tapback_does_not_count_as_reply(void) {
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    /* rowid 5 is an outbound tapback in B's chat after rowid 4. */
    HU_ASSERT_FALSE(hu_imessage_user_replied_after(db, NULL, "+15551110002", 4, never_ours, NULL));
    sqlite3_close(db);
}

static void test_missing_chat_guid_falls_back_to_handle(void) {
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_TRUE(hu_imessage_user_replied_after(db, NULL, "+15551110001", 1, never_ours, NULL));
    HU_ASSERT_TRUE(hu_imessage_user_replied_after(db, "", "+15551110001", 1, never_ours, NULL));
    sqlite3_close(db);
}

static void test_null_db_is_not_replied(void) {
    HU_ASSERT_FALSE(hu_imessage_user_replied_after(NULL, "x", "+15551110001", 1, never_ours, NULL));
}

static void test_zero_rowid_is_not_replied(void) {
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_FALSE(hu_imessage_user_replied_after(db, "iMessage;-;+15551110001", "+15551110001", 0,
                                                   never_ours, NULL));
    sqlite3_close(db);
}

static void test_null_is_ours_counts_every_later_outbound(void) {
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    /* rowid 6's only later outbound is the daemon's, but with no echo-ring
     * callback at all we cannot tell, so it counts (fail toward silence on a
     * replay, which is the incident direction). */
    HU_ASSERT_TRUE(hu_imessage_user_replied_after(db, "iMessage;-;+15551110001", "+15551110001", 6,
                                                  NULL, NULL));
    sqlite3_close(db);
}

static void test_group_chat_any_later_human_bubble_counts(void) {
    /* Documented trade-off: the guard is CHAT-scoped, not reply-to-scoped.
     * Seth's unrelated "running late" (rowid 9) after B's "who's in?" (rowid 8)
     * in the group counts as "human is handling this thread". Suppressing the
     * daemon when the human is visibly active in the thread is the intended
     * bias; a reply-to-guid narrowing would be a separate change. */
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_TRUE(hu_imessage_user_replied_after(db, "iMessage;+;chat123", "+15551110002", 8,
                                                  never_ours, NULL));
    sqlite3_close(db);
}

static void test_attributed_body_only_outbound_counts_as_human(void) {
    /* macOS 15+: human replies often have text NULL and the body in
     * attributedBody. rowid 11 decodes to "yo" and is not ours. */
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_TRUE(hu_imessage_user_replied_after(db, "iMessage;-;+15551110003", "+15551110003", 10,
                                                  never_ours, NULL));
    /* ...and IS excluded when the echo ring recognises the decoded text. */
    HU_ASSERT_FALSE(hu_imessage_user_replied_after(db, "iMessage;-;+15551110003", "+15551110003",
                                                   10, yo_is_ours, NULL));
    sqlite3_close(db);
}

static void test_unreadable_outbound_still_counts_as_human(void) {
    /* rowid 13: text NULL and attributedBody unparseable. An outbound we
     * cannot read is still an outbound; only a positive "ours" excludes it. */
    sqlite3 *db = open_guard_fixture();
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_TRUE(hu_imessage_user_replied_after(db, "iMessage;-;+15551110004", "+15551110004", 12,
                                                  auto_reply_is_ours, NULL));
    sqlite3_close(db);
}
#endif /* HU_HAS_IMESSAGE && HU_ENABLE_SQLITE */

void run_imessage_replay_guard_tests(void) {
    HU_TEST_SUITE("iMessage Replay Guards");
    HU_RUN_TEST(test_resume_no_persisted_seeds_from_db_max);
    HU_RUN_TEST(test_resume_persisted_ahead_of_db_seeds_from_db_max);
    HU_RUN_TEST(test_resume_small_gap_replays_from_persisted);
    HU_RUN_TEST(test_resume_gap_equal_to_cap_still_replays);
    HU_RUN_TEST(test_resume_large_gap_is_capped_to_db_max);
    HU_RUN_TEST(test_resume_null_out_skipped_is_tolerated);
    HU_RUN_TEST(test_stale_unknown_timestamp_is_not_stale);
    HU_RUN_TEST(test_stale_recent_message_is_not_stale);
    HU_RUN_TEST(test_stale_exactly_max_age_is_not_stale);
    HU_RUN_TEST(test_stale_one_past_max_age_is_stale);
    HU_RUN_TEST(test_stale_sixteen_day_old_message_is_stale);
    HU_RUN_TEST(test_stale_zero_max_age_disables_guard);
    HU_RUN_TEST(test_stale_apple_epoch_zero_date_is_unknown_not_stale);
    HU_RUN_TEST(test_env_unset_returns_default);
    HU_RUN_TEST(test_env_valid_number_is_used);
    HU_RUN_TEST(test_env_garbage_falls_back_to_default);
    HU_RUN_TEST(test_replied_guard_applies_to_normal_handle);
    HU_RUN_TEST(test_replied_guard_skips_loopback_handle);
    HU_RUN_TEST(test_replied_guard_null_handle_does_not_apply);
#if HU_HAS_IMESSAGE && defined(HU_ENABLE_SQLITE)
    HU_RUN_TEST(test_replied_after_human_outbound_in_same_chat);
    HU_RUN_TEST(test_not_replied_when_only_our_send_follows);
    HU_RUN_TEST(test_daemon_own_later_send_does_not_count);
    HU_RUN_TEST(test_outbound_in_other_chat_does_not_count);
    HU_RUN_TEST(test_tapback_does_not_count_as_reply);
    HU_RUN_TEST(test_missing_chat_guid_falls_back_to_handle);
    HU_RUN_TEST(test_null_db_is_not_replied);
    HU_RUN_TEST(test_zero_rowid_is_not_replied);
    HU_RUN_TEST(test_null_is_ours_counts_every_later_outbound);
    HU_RUN_TEST(test_group_chat_any_later_human_bubble_counts);
    HU_RUN_TEST(test_attributed_body_only_outbound_counts_as_human);
    HU_RUN_TEST(test_unreadable_outbound_still_counts_as_human);
#endif
}
