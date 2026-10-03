/* tests/test_imessage_reactions.c
 *
 * Phase 2 Task 11 (RL SOTA): unit test for hu_imessage_poll_reactions.
 *
 * Default mode: HU_HAVE_CHATDB undefined → the `_skipped` stub runs.
 * Opt-in mode: -DHU_HAVE_CHATDB=1 + HU_CHATDB env var → exercises the
 *   real SQLite path against a user-supplied chat.db.
 *
 * The forward declaration below is intentional: hu_imessage_poll_reactions
 * is defined in src/channels/imessage.c but not exposed via
 * include/human/channels/imessage.h (test-only consumer for now). */
#include "human/channels/imessage_reactions.h"
#include "human/channels/reaction_event.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

hu_error_t hu_imessage_poll_reactions(const char *db_path, int64_t since_unix,
                                      hu_reaction_event_t *out_events, size_t out_cap,
                                      size_t *out_n);

/* ── reaction_lookup join-key normalization ──────────────────────────────────
 *
 * Regression pins for the zero-imessage_tapback-DPO-pairs bug (2026-05-31 →
 * 2026-07-19). reaction_lookup joins registration and tapback-poll rows on an
 * exact (channel, thread, msg_ref) match, but the two sides derived those keys
 * from different sources and disagreed on spelling, so the join NEVER matched
 * and no training pair was ever recorded.
 *
 * Every literal below is a real value observed in ~/Library/Messages/chat.db
 * and ~/.human/reaction_lookup.db on 2026-07-19. */

static void assoc_guid_strips_part_prefix(void) {
    char out[128];

    /* "p:0/" dominates real data (201 rows), but the part index VARIES —
     * p:1/ and p:4/ both occur. A hardcoded "p:0/" strip would miss them. */
    HU_ASSERT_EQ(hu_imessage_strip_assoc_guid_prefix("p:0/06397B51-F11A-4C4F-B", out, sizeof(out)),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "06397B51-F11A-4C4F-B");

    HU_ASSERT_EQ(hu_imessage_strip_assoc_guid_prefix("p:4/ABC-123", out, sizeof(out)), HU_OK);
    HU_ASSERT_STR_EQ(out, "ABC-123");

    /* ~10% of real rows carry NO prefix — must pass through untouched. */
    HU_ASSERT_EQ(hu_imessage_strip_assoc_guid_prefix("FBCE90F1-5C2B-46BA-A0A3", out, sizeof(out)),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "FBCE90F1-5C2B-46BA-A0A3");

    /* Idempotent: both sides of the join call this, possibly more than once. */
    HU_ASSERT_EQ(hu_imessage_strip_assoc_guid_prefix(out, out, sizeof(out)), HU_OK);
    HU_ASSERT_STR_EQ(out, "FBCE90F1-5C2B-46BA-A0A3");

    /* Must NOT eat a non-part prefix that merely resembles one. */
    HU_ASSERT_EQ(hu_imessage_strip_assoc_guid_prefix("p:/no-digits", out, sizeof(out)), HU_OK);
    HU_ASSERT_STR_EQ(out, "p:/no-digits");
    HU_ASSERT_EQ(hu_imessage_strip_assoc_guid_prefix("px:0/thing", out, sizeof(out)), HU_OK);
    HU_ASSERT_STR_EQ(out, "px:0/thing");
}

static void thread_key_reduces_chat_guid_to_bare_id(void) {
    char out[256];

    /* chat.guid as the poller reads it, vs the bare handle the daemon reply
     * router registers. These two spellings failing to join IS the bug. */
    HU_ASSERT_EQ(hu_imessage_normalize_thread_key("any;-;+14846784914", out, sizeof(out)), HU_OK);
    HU_ASSERT_STR_EQ(out, "+14846784914");

    HU_ASSERT_EQ(hu_imessage_normalize_thread_key("any;-;sethford@me.com", out, sizeof(out)),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "sethford@me.com");

    /* Group chats use the ';+;' kind separator. */
    HU_ASSERT_EQ(hu_imessage_normalize_thread_key("any;+;chat97551123489371982", out, sizeof(out)),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "chat97551123489371982");

    /* Already-bare (registration side) passes through — this is what makes
     * the function safe to apply on both sides of the join. */
    HU_ASSERT_EQ(hu_imessage_normalize_thread_key("+14846784914", out, sizeof(out)), HU_OK);
    HU_ASSERT_STR_EQ(out, "+14846784914");

    /* Idempotent. */
    HU_ASSERT_EQ(hu_imessage_normalize_thread_key(out, out, sizeof(out)), HU_OK);
    HU_ASSERT_STR_EQ(out, "+14846784914");

    /* Legacy per-service prefixes collapse to one key per human, by design. */
    HU_ASSERT_EQ(hu_imessage_normalize_thread_key("SMS;-;+14846784914", out, sizeof(out)), HU_OK);
    HU_ASSERT_STR_EQ(out, "+14846784914");
    HU_ASSERT_EQ(hu_imessage_normalize_thread_key("iMessage;-;+14846784914", out, sizeof(out)),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "+14846784914");
}

/* THE regression test. Asserts the property that was false in production:
 * the key the tapback poller looks up must EQUAL the key registration stored.
 * Fails without the normalizers on either side. */
static void poll_key_matches_registered_key(void) {
    /* What the daemon reply router had at registration time. */
    const char *registered_thread = "+14846784914";
    const char *registered_msg_ref = "06397B51-F11A-4C4F-B";

    /* What chat.db hands the tapback poller for that same message. */
    const char *polled_chat_guid = "any;-;+14846784914";
    const char *polled_assoc_guid = "p:0/06397B51-F11A-4C4F-B";

    char reg_thread[256], poll_thread[256];
    char reg_ref[128], poll_ref[128];

    HU_ASSERT_EQ(
        hu_imessage_normalize_thread_key(registered_thread, reg_thread, sizeof(reg_thread)), HU_OK);
    HU_ASSERT_EQ(
        hu_imessage_normalize_thread_key(polled_chat_guid, poll_thread, sizeof(poll_thread)),
        HU_OK);
    HU_ASSERT_EQ(hu_imessage_strip_assoc_guid_prefix(registered_msg_ref, reg_ref, sizeof(reg_ref)),
                 HU_OK);
    HU_ASSERT_EQ(hu_imessage_strip_assoc_guid_prefix(polled_assoc_guid, poll_ref, sizeof(poll_ref)),
                 HU_OK);

    /* Pre-normalization these differed — the silent join miss. */
    HU_ASSERT_STR_EQ(poll_thread, reg_thread);
    HU_ASSERT_STR_EQ(poll_ref, reg_ref);

    /* And the keys must be the bare forms, not merely equal to each other
     * (guards against a future no-op normalizer making both sides equal by
     * doing nothing). */
    HU_ASSERT_STR_EQ(poll_thread, "+14846784914");
    HU_ASSERT_STR_EQ(poll_ref, "06397B51-F11A-4C4F-B");
}

#ifndef HU_HAVE_CHATDB
static void test_imessage_poll_reactions_skipped(void) {
    fprintf(stderr, "[skip] HU_HAVE_CHATDB not defined\n");
}
#else
static void test_imessage_poll_reactions_returns_recent_tapbacks(void) {
    hu_reaction_event_t events[16] = {0};
    size_t n = 0;
    hu_error_t err =
        hu_imessage_poll_reactions(getenv("HU_CHATDB"), time(NULL) - 86400, events, 16, &n);
    /* hu_imessage_poll_reactions strdup's target_thread_id, target_message_ref,
     * sender_handle, and emoji (Phase 2: iOS 17+ custom-emoji glyph) into
     * each event. MUST free or ASan reports leaks. */
    for (size_t i = 0; i < n; i++) {
        free((void *)events[i].target_thread_id);
        free((void *)events[i].target_message_ref);
        free((void *)events[i].sender_handle);
        free((void *)events[i].emoji);
    }
    HU_ASSERT_EQ(err, HU_OK);
}
#endif

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#include <stdlib.h>

/* DEF-8 fixture: the poll resolves WHAT was reacted to. 98% of our sent
 * chat.db rows have text NULL (body in attributedBody), so the join must not
 * depend on text — only on the target GUID's row. Mac time = unix - 978307200,
 * in nanoseconds. Fixture handles are synthetic 555 numbers. */
static void poll_db_resolves_target_owner_and_send_time(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    const char *sql =
        "CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT);"
        "CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT);"
        "CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);"
        "CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT UNIQUE, text TEXT,"
        "  handle_id INTEGER, date INTEGER, is_from_me INTEGER,"
        "  associated_message_type INTEGER DEFAULT 0, associated_message_guid TEXT,"
        "  associated_message_emoji TEXT);"
        "INSERT INTO handle VALUES (1, '+15550002222');"
        "INSERT INTO chat VALUES (1, 'any;-;+15550002222');"
        /* 1: OUR reply, text NULL (Ventura+ shape), sent at unix 1790000000 */
        "INSERT INTO message VALUES (1, 'OURS-GUID-1', NULL, 1, 812000000000000000, 1, 0, NULL, "
        "NULL);"
        /* 2: the contact's own message */
        "INSERT INTO message VALUES (2, 'THEIRS-GUID-2', 'hey', 1, 812000010000000000, 0, 0, "
        "NULL, NULL);"
        /* 3: contact loves OUR reply (p:0/ prefix) */
        "INSERT INTO message VALUES (3, 'R3', NULL, 1, 812000020000000000, 0, 2000, "
        "'p:0/OURS-GUID-1', NULL);"
        /* 4: contact laughs at their OWN message */
        "INSERT INTO message VALUES (4, 'R4', NULL, 1, 812000030000000000, 0, 2003, "
        "'p:0/THEIRS-GUID-2', NULL);"
        /* 5: WE tapback the contact's message — reactor is us */
        "INSERT INTO message VALUES (5, 'R5', NULL, 1, 812000040000000000, 1, 2001, "
        "'p:0/THEIRS-GUID-2', NULL);"
        /* 6: contact likes our reply via the balloon (bp:) form */
        "INSERT INTO message VALUES (6, 'R6', NULL, 1, 812000050000000000, 0, 2001, "
        "'bp:OURS-GUID-1', NULL);"
        "INSERT INTO chat_message_join VALUES (1,1),(1,2),(1,3),(1,4),(1,5),(1,6);";
    char *err = NULL;
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, &err), SQLITE_OK);
    if (err)
        sqlite3_free(err);

    hu_reaction_event_t ev[8];
    memset(ev, 0, sizeof(ev));
    size_t n = 0;
    HU_ASSERT_EQ(hu_imessage_poll_reactions_db(db, 1789999000, ev, 8, &n), HU_OK);
    HU_ASSERT_EQ(n, 4);
    /* ORDER BY date DESC: R6, R5, R4, R3 */
    const int64_t ours_sent_ms = 812000000000LL + 978307200000LL;
    HU_ASSERT_STR_EQ(ev[0].target_message_ref, "OURS-GUID-1"); /* bp: stripped */
    HU_ASSERT_EQ(ev[0].target_is_ours, 1);
    HU_ASSERT_EQ(ev[0].target_sent_ms, ours_sent_ms);
    HU_ASSERT_EQ(ev[0].target_rowid, 1);
    HU_ASSERT_EQ(ev[0].target_prev_own_rowid, 0); /* nothing of ours before it */
    HU_ASSERT_EQ(ev[1].target_is_ours, 0);        /* we reacted: not a contact signal */
    HU_ASSERT_EQ(ev[2].target_is_ours, 0);        /* their own message */
    HU_ASSERT_STR_EQ(ev[3].target_message_ref, "OURS-GUID-1");
    HU_ASSERT_EQ(ev[3].target_is_ours, 1);
    HU_ASSERT_EQ(ev[3].target_sent_ms, ours_sent_ms);
    HU_ASSERT_EQ(ev[2].target_rowid, 2);
    HU_ASSERT_EQ(ev[2].target_prev_own_rowid, 1); /* our reply precedes their message */
    HU_ASSERT_STR_EQ(ev[3].target_thread_id, "+15550002222");
    for (size_t i = 0; i < n; i++) {
        free((void *)ev[i].target_thread_id);
        free((void *)ev[i].target_message_ref);
        free((void *)ev[i].sender_handle);
        free((void *)ev[i].emoji);
    }
    sqlite3_close(db);
}
#endif

void run_imessage_reactions_tests(void) {
    HU_TEST_SUITE("imessage_reactions");
    HU_RUN_TEST(assoc_guid_strips_part_prefix);
    HU_RUN_TEST(thread_key_reduces_chat_guid_to_bare_id);
    HU_RUN_TEST(poll_key_matches_registered_key);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(poll_db_resolves_target_owner_and_send_time);
#endif
#ifdef HU_HAVE_CHATDB
    HU_RUN_TEST(test_imessage_poll_reactions_returns_recent_tapbacks);
#else
    HU_RUN_TEST(test_imessage_poll_reactions_skipped);
#endif
}
