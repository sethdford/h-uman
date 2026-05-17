/* src/channels/imessage_reactions.c
 *
 * Phase 2 Task 11 (RL SOTA): hu_imessage_poll_reactions — read tapback
 * reactions from a chat.db `message` table and emit normalized
 * hu_reaction_event_t rows.
 *
 * This file is split out from src/channels/imessage.c on purpose:
 * human/channels/reaction_event.h (introduced by Task 10) reuses the
 * enumerator names HU_REACTION_QUESTION and HU_REACTION_CUSTOM_EMOJI,
 * which already exist in the older hu_reaction_type_t enum declared in
 * human/channel.h. Pulling both headers into the same translation unit
 * is a compile error. Task 11 cannot modify reaction_event.h or
 * channel.h, so the new function lives here in a TU that only includes
 * reaction_event.h, never channel.h. See the navigation comment near
 * the original hu_imessage_poll for the breadcrumb.
 *
 * Apple stores message.date as nanoseconds since 2001-01-01 (the Apple
 * "mac_time" reference epoch). We convert to/from Unix seconds with
 * the +978307200 offset.
 *
 * The function strdup's target_thread_id, target_message_ref, and
 * sender_handle into each event — the caller MUST free these. The
 * canonical free loop is pinned in tests/test_imessage_reactions.c. */

#include "human/channels/reaction_event.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

hu_error_t hu_imessage_poll_reactions(const char *db_path, int64_t since_unix,
                                      hu_reaction_event_t *out, size_t cap, size_t *out_n) {
    if (!db_path || !out || !out_n) return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;
#if HU_IS_TEST || !defined(__APPLE__) || !defined(__MACH__) || !defined(HU_ENABLE_SQLITE)
    (void)since_unix;
    (void)cap;
    return HU_ERR_NOT_SUPPORTED;
#else
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return HU_ERR_IO;
    }
    /* iMessage reactions: associated_message_type 2000-2006 (add) or
     * 3000-3006 (remove) with associated_message_guid pointing at the
     * original message. The outer parens around the OR are required for
     * SQL precedence — without them, the AND below would bind tighter
     * and silently drop the 3xxx removal rows. */
    const char *sql =
        "SELECT m.associated_message_type, m.associated_message_guid, "
        "       m.handle_id, h.id, c.guid, m.date "
        "FROM message m "
        "JOIN chat_message_join cmj ON cmj.message_id = m.ROWID "
        "JOIN chat c ON c.ROWID = cmj.chat_id "
        "LEFT JOIN handle h ON h.ROWID = m.handle_id "
        "WHERE (m.associated_message_type BETWEEN 2000 AND 2006 OR m.associated_message_type BETWEEN 3000 AND 3006) "
        "  AND m.associated_message_guid IS NOT NULL "
        "  AND m.date > ((? - 978307200) * 1000000000) "
        "ORDER BY m.date DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_bind_int64(stmt, 1, since_unix);
    sqlite3_bind_int(stmt, 2, (int)cap);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_n < cap) {
        int code = sqlite3_column_int(stmt, 0);
        const unsigned char *guid = sqlite3_column_text(stmt, 1);
        const unsigned char *handle = sqlite3_column_text(stmt, 3);
        const unsigned char *chat_guid = sqlite3_column_text(stmt, 4);
        int64_t mac_ns = sqlite3_column_int64(stmt, 5);

        hu_reaction_kind_t k = HU_REACTION_UNKNOWN;
        hu_reaction_polarity_t p = HU_REACTION_NEUTRAL;
        if (hu_reaction_normalize_imessage(code, &k, &p) != HU_OK) continue;

        out[*out_n].channel_id = "imessage";
        out[*out_n].target_thread_id = chat_guid ? strdup((const char *)chat_guid) : NULL;
        out[*out_n].target_message_ref = guid ? strdup((const char *)guid) : NULL;
        out[*out_n].sender_handle = handle ? strdup((const char *)handle) : NULL;
        out[*out_n].kind = k;
        out[*out_n].polarity = p;
        out[*out_n].timestamp_unix = (mac_ns / 1000000000) + 978307200;
        out[*out_n].is_removal = code >= 3000 ? 1 : 0;
        (*out_n)++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return HU_OK;
#endif
}
