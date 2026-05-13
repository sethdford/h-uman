#ifndef HU_IMESSAGE_POLL_SQL_H
#define HU_IMESSAGE_POLL_SQL_H

/*
 * imessage_poll_sql.h — chat.db poll query fragments.
 *
 * Extracted from imessage_poll.c so the SQL is:
 *   1. Reviewable in isolation. The full query is ~75 lines of nested
 *      EXISTS / COALESCE / CASE — fragile to chat.db schema drift across
 *      macOS releases. Keeping it in its own header makes "what SQL does
 *      the poll run?" a one-file question.
 *   2. Diffable when the schema changes. A future "macOS 27 dropped the
 *      attributedBody column" hotfix is a 5-line edit here, not a
 *      surgical change buried in 410 LOC of the poll function.
 *   3. Snapshot-testable. Tests can `#include` this header and assert
 *      properties of the final assembled query (e.g. "the
 *      has_date_retracted variant always includes was_retracted").
 *
 * Composition
 * -----------
 * The final SQL is concatenated from three or four fragments, chosen
 * by `imessage_poll_has_date_retracted_column()`:
 *
 *     BASE + [RETRACT?] + CHAT_ID + FROM
 *
 *  - BASE     — SELECT clause: ROWID, guid, text (with audio/video/photo
 *               COALESCE), handle id, participant count, attachment
 *               EXISTS flags, edited flag, thread originator, attributed
 *               body, balloon bundle, expressive style, unix timestamp.
 *  - RETRACT  — optional `was_retracted` column (added only when chat.db
 *               has the `date_retracted` column, macOS 13+).
 *  - CHAT_ID  — `chat_guid` from chat_message_join + chat tables.
 *  - FROM     — FROM/WHERE: filters by handle, message type, ROWID >
 *               last seen, and content/attachment/balloon present.
 *
 * Column index mapping (used by imessage_poll.c message parser)
 * -------------------------------------------------------------
 *   0  ROWID                  9  was_edited
 *   1  guid                  10  thread_originator_guid
 *   2  text                  11  attributedBody (BLOB)
 *   3  handle id             12  balloon_bundle_id
 *   4  participant_count     13  expressive_send_style_id
 *   5  has_image             14  unix_ts
 *   6  has_video             15  was_retracted (if RETRACT included)
 *   7  has_audio             15 or 16  chat_guid
 *   8  (unused, reserved)
 *
 * Test coverage
 * -------------
 * tests/test_imessage_chatdb_fixture.c exercises the final query against
 * a synthetic chat.db with known rows.
 */

/* SELECT clause. */
#define IMSG_POLL_SQL_BASE                                                                  \
    "SELECT m.ROWID, m.guid, "                                                              \
    "  COALESCE(m.text, "                                                                   \
    "    (SELECT CASE "                                                                     \
    "       WHEN EXISTS (SELECT 1 FROM message_attachment_join maja "                       \
    "             JOIN attachment aa ON maja.attachment_id = aa.ROWID "                     \
    "             WHERE maja.message_id = m.ROWID AND aa.filename IS NOT NULL "             \
    "             AND (LOWER(aa.filename) LIKE '%.caf' OR LOWER(aa.filename) LIKE '%.m4a' " \
    "               OR LOWER(aa.filename) LIKE '%.mp3' OR LOWER(aa.filename) LIKE '%.aac' " \
    "               OR LOWER(aa.filename) LIKE '%.opus')) "                                 \
    "       THEN '[Voice Message]' "                                                        \
    "       WHEN EXISTS (SELECT 1 FROM message_attachment_join majv "                       \
    "             JOIN attachment av ON majv.attachment_id = av.ROWID "                     \
    "             WHERE majv.message_id = m.ROWID AND av.filename IS NOT NULL "             \
    "             AND (LOWER(av.filename) LIKE '%.mov' OR LOWER(av.filename) LIKE '%.mp4' " \
    "               OR LOWER(av.filename) LIKE '%.m4v')) "                                  \
    "       THEN '[Video]' ELSE '[Photo]' END)) AS text, h.id, "                            \
    "  COALESCE("                                                                           \
    "    (SELECT COUNT(DISTINCT chj2.handle_id) FROM chat_message_join cmj "                \
    "     JOIN chat_handle_join chj2 ON chj2.chat_id = cmj.chat_id "                        \
    "     WHERE cmj.message_id = m.ROWID), 0) AS participant_count, "                       \
    "  EXISTS (SELECT 1 FROM message_attachment_join maj "                                  \
    "   JOIN attachment a ON maj.attachment_id = a.ROWID "                                  \
    "   WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "                         \
    "   AND (LOWER(a.filename) LIKE '%.jpg' OR LOWER(a.filename) LIKE '%.jpeg' "            \
    "     OR LOWER(a.filename) LIKE '%.png' OR LOWER(a.filename) LIKE '%.heic' "            \
    "     OR LOWER(a.filename) LIKE '%.gif' OR LOWER(a.filename) LIKE '%.webp')) "          \
    "   AS has_image, "                                                                     \
    "  EXISTS (SELECT 1 FROM message_attachment_join maj2 "                                 \
    "   JOIN attachment a2 ON maj2.attachment_id = a2.ROWID "                               \
    "   WHERE maj2.message_id = m.ROWID AND a2.filename IS NOT NULL "                       \
    "   AND (LOWER(a2.filename) LIKE '%.mov' OR LOWER(a2.filename) LIKE '%.mp4' "           \
    "     OR LOWER(a2.filename) LIKE '%.m4v')) AS has_video, "                              \
    "  EXISTS (SELECT 1 FROM message_attachment_join maj3 "                                 \
    "   JOIN attachment a3 ON maj3.attachment_id = a3.ROWID "                               \
    "   WHERE maj3.message_id = m.ROWID AND a3.filename IS NOT NULL "                       \
    "   AND (LOWER(a3.filename) LIKE '%.caf' OR LOWER(a3.filename) LIKE '%.m4a' "           \
    "     OR LOWER(a3.filename) LIKE '%.mp3' OR LOWER(a3.filename) LIKE '%.aac' "           \
    "     OR LOWER(a3.filename) LIKE '%.opus')) AS has_audio, "                             \
    "  CASE WHEN m.date_edited > 0 THEN 1 ELSE 0 END AS was_edited, "                       \
    "  m.thread_originator_guid, "                                                          \
    "  m.attributedBody, "                                                                  \
    "  m.balloon_bundle_id, "                                                               \
    "  m.expressive_send_style_id, "                                                        \
    "  m.date / 1000000000 + 978307200 AS unix_ts"

/* Optional was_retracted column (chat.db on macOS 13+). */
#define IMSG_POLL_SQL_RETRACT ", CASE WHEN m.date_retracted > 0 THEN 1 ELSE 0 END AS was_retracted"

/* Chat GUID (group / 1:1 identifier from chat_message_join). */
#define IMSG_POLL_SQL_CHAT_ID                       \
    ", (SELECT c.guid FROM chat_message_join cmj2 " \
    "   JOIN chat c ON cmj2.chat_id = c.ROWID "     \
    "   WHERE cmj2.message_id = m.ROWID LIMIT 1) AS chat_guid"

/* FROM/WHERE clause: filters by handle, message type, ROWID watermark,
 * and content/attachment/balloon presence. Bind order:
 *   ?1 = last-seen ROWID watermark
 *   ?2 = LIMIT (max messages per poll)
 *   ?3 = self-handle id (for is_from_me=1 echo suppression) */
#define IMSG_POLL_SQL_FROM                                                              \
    " FROM message m "                                                                  \
    "JOIN handle h ON m.handle_id = h.ROWID "                                           \
    "WHERE (m.is_from_me = 0 OR (m.is_from_me = 1 AND h.id = ?3)) "                     \
    "AND m.associated_message_type = 0 "                                                \
    "AND m.ROWID > ?1 "                                                                 \
    "AND ((m.text IS NOT NULL AND LENGTH(m.text) > 0) "                                 \
    "     OR (m.attributedBody IS NOT NULL AND LENGTH(m.attributedBody) > 0) "          \
    "     OR (EXISTS (SELECT 1 FROM message_attachment_join maj "                       \
    "         JOIN attachment a ON maj.attachment_id = a.ROWID "                        \
    "         WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "               \
    "         AND ((LOWER(a.filename) LIKE '%.jpg' OR LOWER(a.filename) LIKE '%.jpeg' " \
    "           OR LOWER(a.filename) LIKE '%.png' OR LOWER(a.filename) LIKE '%.heic' "  \
    "           OR LOWER(a.filename) LIKE '%.gif' OR LOWER(a.filename) LIKE '%.webp') " \
    "           OR (LOWER(a.filename) LIKE '%.mov' OR LOWER(a.filename) LIKE '%.mp4' "  \
    "             OR LOWER(a.filename) LIKE '%.m4v') "                                  \
    "           OR (LOWER(a.filename) LIKE '%.caf' OR LOWER(a.filename) LIKE '%.m4a' "  \
    "             OR LOWER(a.filename) LIKE '%.mp3' OR LOWER(a.filename) LIKE '%.aac' " \
    "             OR LOWER(a.filename) LIKE '%.opus')))) "                              \
    "     OR (m.balloon_bundle_id IS NOT NULL)) "                                       \
    "ORDER BY m.ROWID ASC LIMIT ?2"

#endif /* HU_IMESSAGE_POLL_SQL_H */
