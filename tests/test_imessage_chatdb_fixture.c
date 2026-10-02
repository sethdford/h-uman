/* Minimal chat.db fixture for iMessage SQL regression testing.
 * Creates an in-memory SQLite database with the iMessage schema and seed data,
 * then runs the same queries used by imessage.c to catch schema/column drift. */
#if HU_HAS_IMESSAGE && defined(HU_ENABLE_SQLITE)
#include "human/channels/imessage.h"
#include "human/channels/imessage_chat_kind.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>

static const char *schema_sql = "CREATE TABLE handle ("
                                "  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "  id TEXT UNIQUE NOT NULL"
                                ");"
                                "CREATE TABLE message ("
                                "  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "  guid TEXT UNIQUE,"
                                "  text TEXT,"
                                "  handle_id INTEGER,"
                                "  date INTEGER DEFAULT 0,"
                                "  is_from_me INTEGER DEFAULT 0,"
                                "  associated_message_type INTEGER DEFAULT 0,"
                                "  associated_message_guid TEXT,"
                                "  attributedBody BLOB,"
                                "  date_delivered INTEGER DEFAULT 0,"
                                "  date_read INTEGER DEFAULT 0,"
                                "  date_edited INTEGER DEFAULT 0,"
                                "  date_retracted INTEGER DEFAULT 0,"
                                "  thread_originator_guid TEXT,"
                                "  balloon_bundle_id TEXT,"
                                "  expressive_send_style_id TEXT"
                                ");"
                                "CREATE TABLE attachment ("
                                "  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "  filename TEXT"
                                ");"
                                "CREATE TABLE message_attachment_join ("
                                "  message_id INTEGER,"
                                "  attachment_id INTEGER"
                                ");"
                                "CREATE TABLE chat ("
                                "  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "  guid TEXT UNIQUE,"
                                "  style INTEGER"
                                ");"
                                "CREATE TABLE chat_message_join ("
                                "  chat_id INTEGER,"
                                "  message_id INTEGER"
                                ");"
                                "CREATE TABLE chat_handle_join ("
                                "  chat_id INTEGER,"
                                "  handle_id INTEGER"
                                ");";

static const char *seed_sql =
    "INSERT INTO handle (id) VALUES ('+15559999999');"
    "INSERT INTO handle (id) VALUES ('user@example.com');"
    "INSERT INTO chat (guid) VALUES ('iMessage;-;+15559999999');"
    "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (1, 1);"
    /* Inbound text message from +15559999999 */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me, associated_message_type)"
    "  VALUES ('MSG-001', 'Hello there', 1, 700000000000000000, 0, 0);"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1, 1);"
    /* Outbound message */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me, associated_message_type)"
    "  VALUES ('MSG-002', 'Hi back', 1, 700000001000000000, 1, 0);"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1, 2);"
    /* Tapback on MSG-002 (heart=2000) */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me,"
    "  associated_message_type, associated_message_guid)"
    "  VALUES ('MSG-003', NULL, 1, 700000002000000000, 0, 2000, 'MSG-002');"
    /* Message with attachment */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me, associated_message_type)"
    "  VALUES ('MSG-004', NULL, 1, 700000003000000000, 0, 0);"
    "INSERT INTO attachment (filename) VALUES ('~/Library/Messages/photo.jpg');"
    "INSERT INTO message_attachment_join (message_id, attachment_id) VALUES (4, 1);"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1, 4);"
    /* Sticker message (balloon_bundle_id) */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me,"
    "  associated_message_type, balloon_bundle_id)"
    "  VALUES ('MSG-005', NULL, 1, 700000004000000000, 0, 0,"
    "  "
    "'com.apple.messages.MSMessageExtensionBalloonPlugin:0000000000:com.apple.Stickers."
    "UserGenerated.StickerPack');"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1, 5);"
    /* Audio attachment (voice message) */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me, associated_message_type)"
    "  VALUES ('MSG-006', NULL, 1, 700000005000000000, 0, 0);"
    "INSERT INTO attachment (filename) VALUES ('~/Library/Messages/Attachments/voice.caf');"
    "INSERT INTO message_attachment_join (message_id, attachment_id) VALUES (6, 2);"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1, 6);"
    /* Video attachment */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me, associated_message_type)"
    "  VALUES ('MSG-007', NULL, 1, 700000006000000000, 0, 0);"
    "INSERT INTO attachment (filename) VALUES ('~/Library/Messages/Attachments/clip.mov');"
    "INSERT INTO message_attachment_join (message_id, attachment_id) VALUES (7, 3);"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1, 7);"
    /* PDF attachment (unlisted type — should appear as [Attachment]) */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me, associated_message_type)"
    "  VALUES ('MSG-008', NULL, 1, 700000007000000000, 0, 0);"
    "INSERT INTO attachment (filename) VALUES ('~/Library/Messages/Attachments/doc.pdf');"
    "INSERT INTO message_attachment_join (message_id, attachment_id) VALUES (8, 4);"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1, 8);"
    /* Text message with attributedBody only (macOS 15+ scenario) */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me,"
    "  associated_message_type, attributedBody)"
    "  VALUES ('MSG-009', NULL, 1, 700000008000000000, 0, 0, X'62706C697374');"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1, 9);"
    /* Message with Slam effect */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me,"
    "  associated_message_type, expressive_send_style_id)"
    "  VALUES ('MSG-010', 'Wow!', 1, 700000009000000000, 0, 0,"
    "  'com.apple.MobileSMS.expressivesend.impact');"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1, 10);"
    /* Memoji message (balloon_bundle_id with Animoji) */
    "INSERT INTO message (guid, text, handle_id, date, is_from_me,"
    "  associated_message_type, balloon_bundle_id)"
    "  VALUES ('MSG-011', NULL, 1, 700000010000000000, 0, 0,"
    "  'com.apple.messages.MSMessageExtensionBalloonPlugin:0000000000:"
    "com.apple.Animoji.StickersApp.MessagesExtension');"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1, 11);";

static sqlite3 *open_fixture(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK || !db)
        return NULL;
    char *err = NULL;
    rc = sqlite3_exec(db, schema_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    rc = sqlite3_exec(db, seed_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static void test_chatdb_schema_creates_successfully(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);
    sqlite3_close(db);
}

static void test_chatdb_poll_query_returns_inbound(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Production poll SQL from imessage.c — kept in sync to catch drift.
     * COALESCE: voice→[Voice Message], video→[Video], fallback→[Photo]. */
    const char *sql =
        "SELECT m.ROWID, m.guid, "
        "  COALESCE(m.text, "
        "    (SELECT CASE "
        "       WHEN (SELECT COUNT(*) FROM message_attachment_join maja "
        "             JOIN attachment aa ON maja.attachment_id = aa.ROWID "
        "             WHERE maja.message_id = m.ROWID AND aa.filename IS NOT NULL "
        "             AND (LOWER(aa.filename) LIKE '%.caf' OR LOWER(aa.filename) LIKE '%.m4a' "
        "               OR LOWER(aa.filename) LIKE '%.mp3' OR LOWER(aa.filename) LIKE '%.aac' "
        "               OR LOWER(aa.filename) LIKE '%.opus')) > 0 "
        "       THEN '[Voice Message]' "
        "       WHEN (SELECT COUNT(*) FROM message_attachment_join majv "
        "             JOIN attachment av ON majv.attachment_id = av.ROWID "
        "             WHERE majv.message_id = m.ROWID AND av.filename IS NOT NULL "
        "             AND (LOWER(av.filename) LIKE '%.mov' OR LOWER(av.filename) LIKE '%.mp4' "
        "               OR LOWER(av.filename) LIKE '%.m4v')) > 0 "
        "       THEN '[Video]' ELSE '[Photo]' END)) AS text, h.id, "
        "  COALESCE("
        "    (SELECT COUNT(DISTINCT chj2.handle_id) FROM chat_message_join cmj "
        "     JOIN chat_handle_join chj2 ON chj2.chat_id = cmj.chat_id "
        "     WHERE cmj.message_id = m.ROWID), 0) AS participant_count, "
        "  (SELECT COUNT(*) FROM message_attachment_join maj "
        "   JOIN attachment a ON maj.attachment_id = a.ROWID "
        "   WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "
        "   AND (LOWER(a.filename) LIKE '%.jpg' OR LOWER(a.filename) LIKE '%.jpeg' "
        "     OR LOWER(a.filename) LIKE '%.png' OR LOWER(a.filename) LIKE '%.heic' "
        "     OR LOWER(a.filename) LIKE '%.gif' OR LOWER(a.filename) LIKE '%.webp')) "
        "   > 0 AS has_image, "
        "  (SELECT COUNT(*) FROM message_attachment_join maj2 "
        "   JOIN attachment a2 ON maj2.attachment_id = a2.ROWID "
        "   WHERE maj2.message_id = m.ROWID AND a2.filename IS NOT NULL "
        "   AND (LOWER(a2.filename) LIKE '%.mov' OR LOWER(a2.filename) LIKE '%.mp4' "
        "     OR LOWER(a2.filename) LIKE '%.m4v')) > 0 AS has_video, "
        "  (SELECT COUNT(*) FROM message_attachment_join maj3 "
        "   JOIN attachment a3 ON maj3.attachment_id = a3.ROWID "
        "   WHERE maj3.message_id = m.ROWID AND a3.filename IS NOT NULL "
        "   AND (LOWER(a3.filename) LIKE '%.caf' OR LOWER(a3.filename) LIKE '%.m4a' "
        "     OR LOWER(a3.filename) LIKE '%.mp3' OR LOWER(a3.filename) LIKE '%.aac' "
        "     OR LOWER(a3.filename) LIKE '%.opus')) > 0 AS has_audio, "
        "  CASE WHEN m.date_edited > 0 THEN 1 ELSE 0 END AS was_edited, "
        "  m.thread_originator_guid, "
        "  m.attributedBody, "
        "  m.balloon_bundle_id, "
        "  m.expressive_send_style_id "
        "FROM message m "
        "JOIN handle h ON m.handle_id = h.ROWID "
        "WHERE m.is_from_me = 0 AND m.associated_message_type = 0 "
        "AND m.ROWID > ? "
        "AND ((m.text IS NOT NULL AND LENGTH(m.text) > 0) "
        "     OR (m.attributedBody IS NOT NULL AND LENGTH(m.attributedBody) > 0) "
        "     OR (EXISTS (SELECT 1 FROM message_attachment_join maj "
        "         JOIN attachment a ON maj.attachment_id = a.ROWID "
        "         WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "
        "         AND ((LOWER(a.filename) LIKE '%.jpg' OR LOWER(a.filename) LIKE '%.jpeg' "
        "           OR LOWER(a.filename) LIKE '%.png' OR LOWER(a.filename) LIKE '%.heic' "
        "           OR LOWER(a.filename) LIKE '%.gif' OR LOWER(a.filename) LIKE '%.webp') "
        "           OR (LOWER(a.filename) LIKE '%.mov' OR LOWER(a.filename) LIKE '%.mp4' "
        "             OR LOWER(a.filename) LIKE '%.m4v') "
        "           OR (LOWER(a.filename) LIKE '%.caf' OR LOWER(a.filename) LIKE '%.m4a' "
        "             OR LOWER(a.filename) LIKE '%.mp3' OR LOWER(a.filename) LIKE '%.aac' "
        "             OR LOWER(a.filename) LIKE '%.opus')))) "
        "     OR (m.balloon_bundle_id IS NOT NULL)) "
        "ORDER BY m.ROWID ASC LIMIT ?";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, 0);
    sqlite3_bind_int(stmt, 2, 20);

    int count = 0;
    bool saw_voice = false, saw_video = false, saw_photo = false;
    bool saw_text = false, saw_attr = false, saw_effect = false, saw_memoji = false;
    bool saw_has_image = false, saw_has_video = false, saw_has_audio = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(stmt, 2);
        int has_image = sqlite3_column_int(stmt, 5);
        int has_video_col = sqlite3_column_int(stmt, 6);
        int has_audio = sqlite3_column_int(stmt, 7);
        const char *balloon = (const char *)sqlite3_column_text(stmt, 11);
        const char *effect = (const char *)sqlite3_column_text(stmt, 12);
        if (text) {
            if (strcmp(text, "[Voice Message]") == 0)
                saw_voice = true;
            if (strcmp(text, "[Video]") == 0)
                saw_video = true;
            if (strcmp(text, "[Photo]") == 0)
                saw_photo = true;
            if (strcmp(text, "Hello there") == 0)
                saw_text = true;
            if (strcmp(text, "Wow!") == 0 && effect)
                saw_effect = true;
        }
        if (balloon && strstr(balloon, "Animoji"))
            saw_memoji = true;
        {
            const void *ab = sqlite3_column_blob(stmt, 10);
            if (ab && sqlite3_column_bytes(stmt, 10) > 0)
                saw_attr = true;
        }
        if (has_image)
            saw_has_image = true;
        if (has_video_col)
            saw_has_video = true;
        if (has_audio)
            saw_has_audio = true;
        count++;
    }
    /* MSG-001(text), MSG-004(photo→[Photo]), MSG-005(sticker/balloon),
     * MSG-006(voice), MSG-007(video), MSG-009(attributedBody),
     * MSG-010(text with Slam effect), MSG-011(Memoji/balloon).
     * MSG-008(doc.pdf) excluded — production EXISTS filter requires
     * known image/video/audio extensions. */
    HU_ASSERT_EQ(count, 8);
    HU_ASSERT_TRUE(saw_text);
    HU_ASSERT_TRUE(saw_voice);
    HU_ASSERT_TRUE(saw_video);
    HU_ASSERT_TRUE(saw_photo);
    HU_ASSERT_TRUE(saw_attr);
    HU_ASSERT_TRUE(saw_effect);
    HU_ASSERT_TRUE(saw_memoji);
    HU_ASSERT_TRUE(saw_has_image);
    HU_ASSERT_TRUE(saw_has_video);
    HU_ASSERT_TRUE(saw_has_audio);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_tapback_query_counts_reactions(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql = "SELECT m.associated_message_type, COUNT(*) "
                      "FROM message m "
                      "WHERE m.is_from_me = 0 "
                      "  AND m.associated_message_type BETWEEN 2000 AND 2006 "
                      "  AND m.associated_message_guid IN ("
                      "    SELECT m2.guid FROM message m2 "
                      "    WHERE m2.is_from_me = 1 "
                      "    AND m2.handle_id = (SELECT ROWID FROM handle WHERE id = ?1) "
                      "    ORDER BY m2.date DESC LIMIT 5"
                      "  ) "
                      "GROUP BY m.associated_message_type";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "+15559999999", -1, NULL);

    int total = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int type = sqlite3_column_int(stmt, 0);
        int cnt = sqlite3_column_int(stmt, 1);
        HU_ASSERT_EQ(type, 2000);
        total += cnt;
    }
    HU_ASSERT_EQ(total, 1);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_history_query_returns_both_directions(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql = "SELECT m.is_from_me, m.text, m.attributedBody "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.associated_message_type = 0 "
                      "ORDER BY m.date DESC LIMIT 10";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "+15559999999", -1, NULL);

    int inbound = 0, outbound = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_int(stmt, 0) == 0)
            inbound++;
        else
            outbound++;
    }
    HU_ASSERT_TRUE(inbound > 0);
    HU_ASSERT_TRUE(outbound > 0);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

/* load_conversation_history must return the 1:1 thread only. Joining on the
 * handle alone pulled the contact's GROUP messages (and dropped Seth's own
 * 1:1 rows stored with handle_id 0) into the DM context. */
static void test_chatdb_dm_history_excludes_group_messages(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(
        sqlite3_exec(db,
                     "INSERT INTO handle (id) VALUES ('+15550000002');"
                     "INSERT INTO chat (guid, style) VALUES ('iMessage;+;chat-group-1', 43);"
                     "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (2, 1);"
                     "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (2, 3);"
                     "INSERT INTO message (guid, text, handle_id, date, is_from_me) "
                     "  VALUES ('G-1', 'group-only from contact', 1, 790000000000000000, 0);"
                     "INSERT INTO chat_message_join (chat_id, message_id) "
                     "  VALUES (2, (SELECT ROWID FROM message WHERE guid='G-1'));"
                     "INSERT INTO message (guid, text, handle_id, date, is_from_me) "
                     "  VALUES ('G-2', 'group-only from seth', 1, 790000001000000000, 1);"
                     "INSERT INTO chat_message_join (chat_id, message_id) "
                     "  VALUES (2, (SELECT ROWID FROM message WHERE guid='G-2'));"
                     "INSERT INTO message (guid, text, handle_id, date, is_from_me) "
                     "  VALUES ('D-0', 'dm reply with no handle', 0, 790000002000000000, 1);"
                     "INSERT INTO chat_message_join (chat_id, message_id) "
                     "  VALUES (1, (SELECT ROWID FROM message WHERE guid='D-0'));",
                     NULL, NULL, NULL),
        SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, HU_IMESSAGE_SQL_DM_HISTORY, -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "+15559999999", -1, NULL);
    sqlite3_bind_int(stmt, 2, 50);
    int rows = 0, group_rows = 0, dm_seth_no_handle = 0, dm_hello = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows++;
        const char *t = (const char *)sqlite3_column_text(stmt, 1);
        if (t && strstr(t, "group-only"))
            group_rows++;
        if (t && strcmp(t, "dm reply with no handle") == 0)
            dm_seth_no_handle++;
        if (t && strcmp(t, "Hello there") == 0)
            dm_hello++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    HU_ASSERT_GT(rows, 0);
    HU_ASSERT_EQ(dm_hello, 1);
    HU_ASSERT_EQ(group_rows, 0);
    HU_ASSERT_EQ(dm_seth_no_handle, 1);
}

static void test_chatdb_attachment_join_works(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql = "SELECT a.filename FROM attachment a "
                      "JOIN message_attachment_join maj ON maj.attachment_id = a.ROWID "
                      "WHERE maj.message_id = ?1 LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, 4);

    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *fname = (const char *)sqlite3_column_text(stmt, 0);
    HU_ASSERT_NOT_NULL(fname);
    HU_ASSERT_TRUE(strstr(fname, "photo.jpg") != NULL);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_chat_guid_lookup(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql = "SELECT c.guid FROM chat c "
                      "JOIN chat_message_join cmj ON c.ROWID = cmj.chat_id "
                      "WHERE cmj.message_id = ? LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, 1);

    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *chat_guid = (const char *)sqlite3_column_text(stmt, 0);
    HU_ASSERT_STR_EQ(chat_guid, "iMessage;-;+15559999999");

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_max_rowid(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql = "SELECT MAX(ROWID) FROM message";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    int64_t max_rowid = sqlite3_column_int64(stmt, 0);
    HU_ASSERT_TRUE(max_rowid >= 5);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_chat_id_by_message_id_for_react(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql = "SELECT cmj.chat_id FROM chat_message_join cmj "
                      "WHERE cmj.message_id = ? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_bind_int64(stmt, 1, 1);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    int64_t chat_id = sqlite3_column_int64(stmt, 0);
    HU_ASSERT_EQ(chat_id, 1);
    sqlite3_reset(stmt);

    sqlite3_bind_int64(stmt, 1, 999);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_reset(stmt);

    sqlite3_bind_int64(stmt, 1, 4);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    int64_t chat_id4 = sqlite3_column_int64(stmt, 0);
    HU_ASSERT_EQ(chat_id4, 1);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_participant_count_subquery(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql = "SELECT COALESCE("
                      "  (SELECT COUNT(DISTINCT chj2.handle_id) FROM chat_message_join cmj "
                      "   JOIN chat_handle_join chj2 ON chj2.chat_id = cmj.chat_id "
                      "   WHERE cmj.message_id = ?), 0)";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, 1);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    int participants = sqlite3_column_int(stmt, 0);
    HU_ASSERT_EQ(participants, 1);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_balloon_bundle_id_detection(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql = "SELECT m.balloon_bundle_id FROM message m "
                      "WHERE m.ROWID = 5 AND m.balloon_bundle_id IS NOT NULL";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *bundle = (const char *)sqlite3_column_text(stmt, 0);
    HU_ASSERT_NOT_NULL(bundle);
    HU_ASSERT_TRUE(strstr(bundle, "Sticker") != NULL);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_self_react_fallback_query(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql = "SELECT text, guid, attributedBody FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ? AND m.is_from_me = 1 "
                      "AND m.associated_message_type = 0 "
                      "ORDER BY m.ROWID DESC LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "+15559999999", -1, NULL);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *text = (const char *)sqlite3_column_text(stmt, 0);
    HU_ASSERT_STR_EQ(text, "Hi back");

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_attachment_classification(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql =
        "SELECT m.ROWID, "
        "  COALESCE(m.text, "
        "    (SELECT CASE "
        "       WHEN (SELECT COUNT(*) FROM message_attachment_join maja "
        "             JOIN attachment aa ON maja.attachment_id = aa.ROWID "
        "             WHERE maja.message_id = m.ROWID AND aa.filename IS NOT NULL "
        "             AND (LOWER(aa.filename) LIKE '%.caf' OR LOWER(aa.filename) LIKE '%.m4a')) > "
        "0 "
        "       THEN '[Voice Message]' "
        "       WHEN (SELECT COUNT(*) FROM message_attachment_join majv "
        "             JOIN attachment av ON majv.attachment_id = av.ROWID "
        "             WHERE majv.message_id = m.ROWID AND av.filename IS NOT NULL "
        "             AND (LOWER(av.filename) LIKE '%.mov' OR LOWER(av.filename) LIKE '%.mp4')) > "
        "0 "
        "       THEN '[Video]' "
        "       WHEN (SELECT COUNT(*) FROM message_attachment_join majimg "
        "             JOIN attachment aimg ON majimg.attachment_id = aimg.ROWID "
        "             WHERE majimg.message_id = m.ROWID AND aimg.filename IS NOT NULL "
        "             AND (LOWER(aimg.filename) LIKE '%.jpg' OR LOWER(aimg.filename) LIKE "
        "'%.png')) > 0 "
        "       THEN '[Photo]' "
        "       WHEN (SELECT COUNT(*) FROM message_attachment_join "
        "             WHERE message_id = m.ROWID) > 0 "
        "       THEN '[Attachment]' ELSE NULL END)) AS text "
        "FROM message m WHERE m.ROWID IN (4, 6, 7, 8, 9) ORDER BY m.ROWID";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    /* ROWID 4: photo.jpg → [Photo] */
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "[Photo]");

    /* ROWID 6: voice.caf → [Voice Message] */
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "[Voice Message]");

    /* ROWID 7: clip.mov → [Video] */
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "[Video]");

    /* ROWID 8: doc.pdf → [Attachment] */
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "[Attachment]");

    /* ROWID 9: attributedBody only, no attachment → NULL */
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_TRUE(sqlite3_column_text(stmt, 1) == NULL);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_tapback_context_query(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Same SQL as production hu_imessage_build_tapback_context (minus the date filter
     * which uses strftime('now') — we test the structure, not the time window). */
    const char *sql = "SELECT m.associated_message_type, COUNT(*) "
                      "FROM message m "
                      "WHERE m.is_from_me = 0 "
                      "  AND m.associated_message_type BETWEEN 2000 AND 2006 "
                      "  AND m.associated_message_guid IN ("
                      "    SELECT m2.guid FROM message m2 "
                      "    WHERE m2.is_from_me = 1 "
                      "    AND m2.handle_id = (SELECT ROWID FROM handle WHERE id = ?1) "
                      "    ORDER BY m2.date DESC LIMIT 5"
                      "  ) "
                      "GROUP BY m.associated_message_type";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "+15559999999", -1, NULL);

    int hearts = 0, total = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int type = sqlite3_column_int(stmt, 0);
        int cnt = sqlite3_column_int(stmt, 1);
        if (type == 2000)
            hearts = cnt;
        total += cnt;
    }
    HU_ASSERT_EQ(hearts, 1);
    HU_ASSERT_EQ(total, 1);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_read_receipt_query(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Same SQL as production hu_imessage_build_read_receipt_context */
    const char *sql = "SELECT m.date, m.date_delivered, m.date_read, m.text "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 1 "
                      "ORDER BY m.date DESC LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "+15559999999", -1, NULL);

    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *text = (const char *)sqlite3_column_text(stmt, 3);
    HU_ASSERT_STR_EQ(text, "Hi back");

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_gif_tapback_count_query(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Add a GIF attachment on an outbound message, then a tapback on it */
    const char *extra =
        "INSERT INTO message (guid, text, handle_id, date, is_from_me, associated_message_type)"
        "  VALUES ('MSG-GIF-OUT', 'check this out', 1, 700000009000000000, 1, 0);"
        "INSERT INTO attachment (filename) VALUES ('/tmp/human_gif_reaction.gif');"
        "INSERT INTO message_attachment_join (message_id, attachment_id) VALUES ("
        "  (SELECT ROWID FROM message WHERE guid = 'MSG-GIF-OUT'),"
        "  (SELECT ROWID FROM attachment WHERE filename = '/tmp/human_gif_reaction.gif'));"
        "INSERT INTO message (guid, text, handle_id, date, is_from_me,"
        "  associated_message_type, associated_message_guid)"
        "  VALUES ('MSG-GIF-TAP', NULL, 1, 700000010000000000, 0, 2000, 'MSG-GIF-OUT');";
    char *err = NULL;
    sqlite3_exec(db, extra, NULL, NULL, &err);
    if (err)
        sqlite3_free(err);

    /* Same SQL structure as production hu_imessage_count_recent_gif_tapbacks
     * (without the date filter for reproducibility in tests). */
    const char *sql = "SELECT COUNT(*) FROM message m "
                      "WHERE m.is_from_me = 0 "
                      "  AND (m.associated_message_type BETWEEN 2000 AND 2004 "
                      "       OR m.associated_message_type = 2006) "
                      "  AND m.handle_id = (SELECT ROWID FROM handle WHERE id = ?1) "
                      "  AND m.associated_message_guid IN ("
                      "    SELECT m2.guid FROM message m2 "
                      "    JOIN message_attachment_join maj ON maj.message_id = m2.ROWID "
                      "    JOIN attachment a ON maj.attachment_id = a.ROWID "
                      "    WHERE m2.is_from_me = 1 "
                      "      AND LOWER(a.filename) LIKE '%.gif'"
                      "  )";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "+15559999999", -1, NULL);

    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    HU_ASSERT_EQ(count, 1);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_latest_sent_rowid_query(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Same SQL as hu_imessage_get_latest_sent_rowid */
    const char *sql = "SELECT MAX(m.ROWID) FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE m.is_from_me = 1 AND h.id = ?1";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "+15559999999", -1, NULL);

    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_TRUE(sqlite3_column_type(stmt, 0) != SQLITE_NULL);
    int64_t rowid = sqlite3_column_int64(stmt, 0);
    HU_ASSERT_EQ(rowid, 2); /* MSG-002 is our only is_from_me=1 message */

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_voice_msg_has_attachment_flag(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Verify that the poll query sets has_audio=1 for voice.caf (ROWID 6),
     * which should cause has_attachment=true in production (has_image || has_audio). */
    const char *sql = "SELECT "
                      "  (SELECT COUNT(*) FROM message_attachment_join maj3 "
                      "   JOIN attachment a3 ON maj3.attachment_id = a3.ROWID "
                      "   WHERE maj3.message_id = m.ROWID AND a3.filename IS NOT NULL "
                      "   AND (LOWER(a3.filename) LIKE '%.caf' OR LOWER(a3.filename) LIKE '%.m4a' "
                      "     OR LOWER(a3.filename) LIKE '%.mp3' OR LOWER(a3.filename) LIKE '%.aac' "
                      "     OR LOWER(a3.filename) LIKE '%.opus')) > 0 AS has_audio, "
                      "  (SELECT COUNT(*) FROM message_attachment_join maj "
                      "   JOIN attachment a ON maj.attachment_id = a.ROWID "
                      "   WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "
                      "   AND (LOWER(a.filename) LIKE '%.jpg' OR LOWER(a.filename) LIKE '%.jpeg' "
                      "     OR LOWER(a.filename) LIKE '%.png' OR LOWER(a.filename) LIKE '%.heic' "
                      "     OR LOWER(a.filename) LIKE '%.gif' OR LOWER(a.filename) LIKE '%.webp')) "
                      "   > 0 AS has_image "
                      "FROM message m WHERE m.ROWID = 6";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    int has_audio = sqlite3_column_int(stmt, 0);
    int has_image = sqlite3_column_int(stmt, 1);
    HU_ASSERT_EQ(has_audio, 1);
    HU_ASSERT_EQ(has_image, 0);
    /* Production: has_attachment = (has_image != 0 || has_audio != 0) → true */
    HU_ASSERT_TRUE(has_image != 0 || has_audio != 0);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_inline_reply_guid_lookup(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Add a reply that references MSG-001 via thread_originator_guid */
    const char *extra =
        "INSERT INTO message (guid, text, handle_id, date, is_from_me,"
        "  associated_message_type, thread_originator_guid)"
        "  VALUES ('MSG-REPLY', 'I agree!', 1, 700000012000000000, 0, 0, 'MSG-001');"
        "INSERT INTO chat_message_join (chat_id, message_id) VALUES (1,"
        "  (SELECT ROWID FROM message WHERE guid = 'MSG-REPLY'));";
    char *err = NULL;
    sqlite3_exec(db, extra, NULL, NULL, &err);
    if (err)
        sqlite3_free(err);

    /* Look up the original message text by GUID (same pattern as
     * hu_imessage_lookup_message_by_guid) */
    const char *sql = "SELECT m.text FROM message m WHERE m.guid = ?1 LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "MSG-001", -1, NULL);

    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *text = (const char *)sqlite3_column_text(stmt, 0);
    HU_ASSERT_STR_EQ(text, "Hello there");

    sqlite3_finalize(stmt);

    /* Also verify the reply's thread_originator_guid is set */
    const char *sql2 = "SELECT m.thread_originator_guid FROM message m "
                       "WHERE m.guid = 'MSG-REPLY'";
    sqlite3_stmt *stmt2 = NULL;
    rc = sqlite3_prepare_v2(db, sql2, -1, &stmt2, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt2), SQLITE_ROW);
    const char *reply_to = (const char *)sqlite3_column_text(stmt2, 0);
    HU_ASSERT_STR_EQ(reply_to, "MSG-001");

    sqlite3_finalize(stmt2);
    sqlite3_close(db);
}

static void test_chatdb_voice_message_coalesce_text(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Verify COALESCE produces "[Voice Message]" for ROWID 6 (voice.caf, no text) */
    const char *sql =
        "SELECT COALESCE(m.text, "
        "  (SELECT CASE "
        "     WHEN (SELECT COUNT(*) FROM message_attachment_join maja "
        "           JOIN attachment aa ON maja.attachment_id = aa.ROWID "
        "           WHERE maja.message_id = m.ROWID AND aa.filename IS NOT NULL "
        "           AND (LOWER(aa.filename) LIKE '%.caf' OR LOWER(aa.filename) LIKE '%.m4a' "
        "             OR LOWER(aa.filename) LIKE '%.mp3' OR LOWER(aa.filename) LIKE '%.aac' "
        "             OR LOWER(aa.filename) LIKE '%.opus')) > 0 "
        "     THEN '[Voice Message]' "
        "     WHEN (SELECT COUNT(*) FROM message_attachment_join majv "
        "           JOIN attachment av ON majv.attachment_id = av.ROWID "
        "           WHERE majv.message_id = m.ROWID AND av.filename IS NOT NULL "
        "           AND (LOWER(av.filename) LIKE '%.mov' OR LOWER(av.filename) LIKE '%.mp4' "
        "             OR LOWER(av.filename) LIKE '%.m4v')) > 0 "
        "     THEN '[Video]' ELSE '[Photo]' END)) AS text "
        "FROM message m WHERE m.ROWID = 6";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *text = (const char *)sqlite3_column_text(stmt, 0);
    HU_ASSERT_STR_EQ(text, "[Voice Message]");

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_retracted_detection(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Add a retracted message */
    const char *extra =
        "INSERT INTO message (guid, text, handle_id, date, is_from_me,"
        "  associated_message_type, date_retracted)"
        "  VALUES ('MSG-UNSEND', 'oops', 1, 700000013000000000, 0, 0, 700000013500000000);";
    char *err = NULL;
    sqlite3_exec(db, extra, NULL, NULL, &err);
    if (err)
        sqlite3_free(err);

    const char *sql = "SELECT CASE WHEN date_retracted > 0 THEN 1 ELSE 0 END "
                      "FROM message WHERE guid = 'MSG-UNSEND'";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    int was_unsent = sqlite3_column_int(stmt, 0);
    HU_ASSERT_EQ(was_unsent, 1);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_edited_detection(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Add an edited message */
    const char *extra = "INSERT INTO message (guid, text, handle_id, date, is_from_me,"
                        "  associated_message_type, date_edited)"
                        "  VALUES ('MSG-EDIT', 'corrected text', 1, 700000014000000000, 0, 0, "
                        "700000014500000000);";
    char *err = NULL;
    sqlite3_exec(db, extra, NULL, NULL, &err);
    if (err)
        sqlite3_free(err);

    const char *sql = "SELECT CASE WHEN date_edited > 0 THEN 1 ELSE 0 END "
                      "FROM message WHERE guid = 'MSG-EDIT'";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    int was_edited = sqlite3_column_int(stmt, 0);
    HU_ASSERT_EQ(was_edited, 1);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_no_sent_rowid_for_unknown_handle(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    const char *sql = "SELECT MAX(m.ROWID) FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE m.is_from_me = 1 AND h.id = ?1";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "+15550000000", -1, NULL);

    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    /* NULL result for unknown handle */
    HU_ASSERT_EQ(sqlite3_column_type(stmt, 0), SQLITE_NULL);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_optimized_poll_exists_and_inline_retract(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Insert a retracted message to verify inline CASE detection */
    const char *extra = "INSERT INTO message (guid, text, handle_id, date, is_from_me,"
                        "  associated_message_type, date_retracted)"
                        "  VALUES ('MSG-OPT-RETRACT', 'oops', 1, 700000020000000000, 0, 0,"
                        " 700000020500000000);"
                        "INSERT INTO chat_message_join (chat_id, message_id) "
                        "  VALUES (1, (SELECT ROWID FROM message WHERE guid='MSG-OPT-RETRACT'));";
    char *err = NULL;
    sqlite3_exec(db, extra, NULL, NULL, &err);
    if (err)
        sqlite3_free(err);

    /* Optimized poll SQL: EXISTS instead of COUNT, inline retract, chat_guid, unix_ts */
    const char *sql = "SELECT m.ROWID, m.guid, m.text, h.id, "
                      "  COALESCE("
                      "    (SELECT COUNT(DISTINCT chj2.handle_id) FROM chat_message_join cmj "
                      "     JOIN chat_handle_join chj2 ON chj2.chat_id = cmj.chat_id "
                      "     WHERE cmj.message_id = m.ROWID), 0) AS participant_count, "
                      "  EXISTS (SELECT 1 FROM message_attachment_join maj "
                      "   JOIN attachment a ON maj.attachment_id = a.ROWID "
                      "   WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "
                      "   AND LOWER(a.filename) LIKE '%.jpg') AS has_image, "
                      "  CASE WHEN m.date_edited > 0 THEN 1 ELSE 0 END AS was_edited, "
                      "  CASE WHEN m.date_retracted > 0 THEN 1 ELSE 0 END AS was_retracted, "
                      "  m.date / 1000000000 + 978307200 AS unix_ts, "
                      "  (SELECT c.guid FROM chat_message_join cmj2 "
                      "   JOIN chat c ON cmj2.chat_id = c.ROWID "
                      "   WHERE cmj2.message_id = m.ROWID LIMIT 1) AS chat_guid "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE m.is_from_me = 0 AND m.associated_message_type = 0 "
                      "AND m.ROWID > 0 "
                      "ORDER BY m.ROWID ASC LIMIT 20";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    bool found_retracted = false;
    bool found_chat_guid = false;
    bool found_unix_ts = false;
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *guid = (const char *)sqlite3_column_text(stmt, 1);
        int was_retracted = sqlite3_column_int(stmt, 7);
        int64_t unix_ts = sqlite3_column_int64(stmt, 8);
        const char *chat_guid = (const char *)sqlite3_column_text(stmt, 9);

        if (guid && strcmp(guid, "MSG-OPT-RETRACT") == 0) {
            HU_ASSERT_EQ(was_retracted, 1);
            found_retracted = true;
        }
        if (chat_guid && chat_guid[0])
            found_chat_guid = true;
        if (unix_ts > 0)
            found_unix_ts = true;
        count++;
    }
    HU_ASSERT_TRUE(count > 0);
    HU_ASSERT_TRUE(found_retracted);
    HU_ASSERT_TRUE(found_chat_guid);
    HU_ASSERT_TRUE(found_unix_ts);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_unix_timestamp_conversion(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* MSG-001 has date=700000000000000000 (Core Data nanoseconds since 2001-01-01).
     * Expected: 700000000000000000 / 1000000000 + 978307200 = 700000000 + 978307200
     *         = 1678307200 (approximately 2023-03-09) */
    const char *sql = "SELECT m.date / 1000000000 + 978307200 AS unix_ts "
                      "FROM message m WHERE m.guid = 'MSG-001'";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);

    int64_t ts = sqlite3_column_int64(stmt, 0);
    HU_ASSERT_EQ(ts, (int64_t)(700000000LL + 978307200LL));

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void test_chatdb_group_chat_participant_count(void) {
    sqlite3 *db = open_fixture();
    HU_ASSERT_NOT_NULL(db);

    /* Add a group chat with 3 participants */
    const char *extra =
        "INSERT INTO handle (id) VALUES ('group_member_a@test.com');"
        "INSERT INTO chat (guid) VALUES ('iMessage;+;chat_group');"
        "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (2, 1);"
        "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (2, 2);"
        "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (2, 3);"
        "INSERT INTO message (guid, text, handle_id, date, is_from_me, associated_message_type)"
        "  VALUES ('MSG-GRP', 'group hello', 3, 700000030000000000, 0, 0);"
        "INSERT INTO chat_message_join (chat_id, message_id) "
        "  VALUES (2, (SELECT ROWID FROM message WHERE guid='MSG-GRP'));";
    char *err = NULL;
    sqlite3_exec(db, extra, NULL, NULL, &err);
    if (err)
        sqlite3_free(err);

    /* Verify participant count = 3 and chat_guid populated */
    const char *sql = "SELECT "
                      "  COALESCE("
                      "    (SELECT COUNT(DISTINCT chj2.handle_id) FROM chat_message_join cmj "
                      "     JOIN chat_handle_join chj2 ON chj2.chat_id = cmj.chat_id "
                      "     WHERE cmj.message_id = m.ROWID), 0) AS participant_count, "
                      "  (SELECT c.guid FROM chat_message_join cmj2 "
                      "   JOIN chat c ON cmj2.chat_id = c.ROWID "
                      "   WHERE cmj2.message_id = m.ROWID LIMIT 1) AS chat_guid "
                      "FROM message m WHERE m.guid = 'MSG-GRP'";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);

    int pcount = sqlite3_column_int(stmt, 0);
    const char *chat_guid = (const char *)sqlite3_column_text(stmt, 1);
    HU_ASSERT_EQ(pcount, 3);
    HU_ASSERT_NOT_NULL(chat_guid);
    HU_ASSERT_STR_EQ(chat_guid, "iMessage;+;chat_group");

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

/* chat_handle_join never lists the owner: a 1:1 has 1 handle, a 3-person
 * group (owner + 2) has 2. Real chat.db (2026-09-30): all 2,286 style-45
 * chats have exactly 1 handle; 104 style-43 groups have exactly 2. The old
 * `participant_count > 2` test called every one of those 104 a DM. These run
 * the production SQL fragments + classifier against a fixture chat.db. */
static const char *chat_kind_seed =
    "INSERT INTO handle (id) VALUES ('kind_a@test.com');" /* 3 */
    "INSERT INTO handle (id) VALUES ('kind_b@test.com');" /* 4 */
    "INSERT INTO handle (id) VALUES ('kind_c@test.com');" /* 5 */
    /* chat 2: 1:1 with handle 3 */
    "INSERT INTO chat (ROWID, guid, style) VALUES (2, 'iMessage;-;kind_a@test.com', 45);"
    "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (2, 3);"
    /* chat 3: 3-person group = owner + handles 3,4 */
    "INSERT INTO chat (ROWID, guid, style) VALUES (3, 'iMessage;+;chat3person', 43);"
    "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (3, 3);"
    "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (3, 4);"
    /* chat 4: 4-person group = owner + handles 3,4,5 */
    "INSERT INTO chat (ROWID, guid, style) VALUES (4, 'iMessage;+;chat4person', 43);"
    "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (4, 3);"
    "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (4, 4);"
    "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (4, 5);"
    /* chat 5: style unknown (NULL), 2 handles -> fallback must say group */
    "INSERT INTO chat (ROWID, guid, style) VALUES (5, 'iMessage;+;chatnostyle', NULL);"
    "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (5, 4);"
    "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (5, 5);"
    "INSERT INTO message (ROWID, guid, text, handle_id, date) VALUES "
    "  (102, 'KIND-DM', 'hi', 3, 700000040000000000),"
    "  (103, 'KIND-G3', 'hi all', 3, 700000041000000000),"
    "  (104, 'KIND-G4', 'hi all', 3, 700000042000000000),"
    "  (105, 'KIND-NS', 'hi all', 4, 700000043000000000);"
    "INSERT INTO chat_message_join (chat_id, message_id) VALUES "
    "  (2, 102), (3, 103), (4, 104), (5, 105);";

/* Returns 1 group, 0 not group, -1 on query failure. */
static int chat_kind_is_group_for(sqlite3 *db, const char *msg_guid, int *out_handles) {
    const char *sql =
        "SELECT " HU_IMESSAGE_SQL_CHAT_STYLE_OF_MESSAGE ", " HU_IMESSAGE_SQL_HANDLE_COUNT_OF_MESSAGE
        " FROM message m WHERE m.guid = ?1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, msg_guid, -1, SQLITE_STATIC);
    int result = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int style = sqlite3_column_type(stmt, 0) == SQLITE_NULL ? HU_IMESSAGE_CHAT_STYLE_UNKNOWN
                                                                : sqlite3_column_int(stmt, 0);
        int handles = sqlite3_column_int(stmt, 1);
        if (out_handles)
            *out_handles = handles;
        result = hu_imessage_chat_is_group(style, handles) ? 1 : 0;
    }
    sqlite3_finalize(stmt);
    return result;
}

static sqlite3 *open_chat_kind_fixture(void) {
    sqlite3 *db = open_fixture();
    if (!db)
        return NULL;
    char *err = NULL;
    if (sqlite3_exec(db, chat_kind_seed, NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static void test_chatdb_one_to_one_is_not_group(void) {
    sqlite3 *db = open_chat_kind_fixture();
    HU_ASSERT_NOT_NULL(db);
    int handles = -1;
    HU_ASSERT_EQ(chat_kind_is_group_for(db, "KIND-DM", &handles), 0);
    HU_ASSERT_EQ(handles, 1);
    sqlite3_close(db);
}

static void test_chatdb_three_person_group_is_group(void) {
    sqlite3 *db = open_chat_kind_fixture();
    HU_ASSERT_NOT_NULL(db);
    int handles = -1;
    HU_ASSERT_EQ(chat_kind_is_group_for(db, "KIND-G3", &handles), 1);
    HU_ASSERT_EQ(handles, 2); /* owner excluded: 3 people, 2 handles */
    sqlite3_close(db);
}

static void test_chatdb_four_person_group_is_group(void) {
    sqlite3 *db = open_chat_kind_fixture();
    HU_ASSERT_NOT_NULL(db);
    int handles = -1;
    HU_ASSERT_EQ(chat_kind_is_group_for(db, "KIND-G4", &handles), 1);
    HU_ASSERT_EQ(handles, 3);
    sqlite3_close(db);
}

static void test_chatdb_group_fallback_without_style(void) {
    sqlite3 *db = open_chat_kind_fixture();
    HU_ASSERT_NOT_NULL(db);
    int handles = -1;
    HU_ASSERT_EQ(chat_kind_is_group_for(db, "KIND-NS", &handles), 1);
    HU_ASSERT_EQ(handles, 2);
    sqlite3_close(db);
}

static void test_chat_kind_style_decides_over_handle_count(void) {
    /* A known style wins even when the handle count would say otherwise. */
    HU_ASSERT_FALSE(hu_imessage_chat_is_group(HU_IMESSAGE_CHAT_STYLE_DIRECT, 2));
    HU_ASSERT_TRUE(hu_imessage_chat_is_group(HU_IMESSAGE_CHAT_STYLE_GROUP, 1));
    /* Unknown style falls back to handle count (owner excluded). */
    HU_ASSERT_FALSE(hu_imessage_chat_is_group(HU_IMESSAGE_CHAT_STYLE_UNKNOWN, 0));
    HU_ASSERT_FALSE(hu_imessage_chat_is_group(HU_IMESSAGE_CHAT_STYLE_UNKNOWN, 1));
    HU_ASSERT_TRUE(hu_imessage_chat_is_group(HU_IMESSAGE_CHAT_STYLE_UNKNOWN, 2));
}

void run_imessage_chatdb_fixture_tests(void) {
    HU_TEST_SUITE("iMessage ChatDB Fixture");
    HU_RUN_TEST(test_chatdb_schema_creates_successfully);
    HU_RUN_TEST(test_chatdb_poll_query_returns_inbound);
    HU_RUN_TEST(test_chatdb_tapback_query_counts_reactions);
    HU_RUN_TEST(test_chatdb_history_query_returns_both_directions);
    HU_RUN_TEST(test_chatdb_dm_history_excludes_group_messages);
    HU_RUN_TEST(test_chatdb_attachment_join_works);
    HU_RUN_TEST(test_chatdb_chat_guid_lookup);
    HU_RUN_TEST(test_chatdb_max_rowid);
    HU_RUN_TEST(test_chatdb_chat_id_by_message_id_for_react);
    HU_RUN_TEST(test_chatdb_participant_count_subquery);
    HU_RUN_TEST(test_chatdb_balloon_bundle_id_detection);
    HU_RUN_TEST(test_chatdb_self_react_fallback_query);
    HU_RUN_TEST(test_chatdb_attachment_classification);
    HU_RUN_TEST(test_chatdb_tapback_context_query);
    HU_RUN_TEST(test_chatdb_read_receipt_query);
    HU_RUN_TEST(test_chatdb_gif_tapback_count_query);
    HU_RUN_TEST(test_chatdb_latest_sent_rowid_query);
    HU_RUN_TEST(test_chatdb_voice_msg_has_attachment_flag);
    HU_RUN_TEST(test_chatdb_inline_reply_guid_lookup);
    HU_RUN_TEST(test_chatdb_voice_message_coalesce_text);
    HU_RUN_TEST(test_chatdb_retracted_detection);
    HU_RUN_TEST(test_chatdb_edited_detection);
    HU_RUN_TEST(test_chatdb_no_sent_rowid_for_unknown_handle);
    HU_RUN_TEST(test_chatdb_optimized_poll_exists_and_inline_retract);
    HU_RUN_TEST(test_chatdb_unix_timestamp_conversion);
    HU_RUN_TEST(test_chatdb_group_chat_participant_count);
    HU_RUN_TEST(test_chatdb_one_to_one_is_not_group);
    HU_RUN_TEST(test_chatdb_three_person_group_is_group);
    HU_RUN_TEST(test_chatdb_four_person_group_is_group);
    HU_RUN_TEST(test_chatdb_group_fallback_without_style);
    HU_RUN_TEST(test_chat_kind_style_decides_over_handle_count);
}
#else
void run_imessage_chatdb_fixture_tests(void) {
    (void)0;
}
#endif
