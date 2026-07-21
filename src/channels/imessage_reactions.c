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

#include "human/channels/imessage_reactions.h"
#include "human/channels/reaction_event.h"
#include "human/core/error.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Reaction-lookup join-key normalizers ────────────────────────────────────
 *
 * Contract + rationale live in human/channels/imessage_reactions.h. These are
 * pure string predicates (no DB, no allocation) so the join-key contract is
 * unit-testable without a chat.db fixture or a live daemon.
 *
 * These exist because the registration and lookup sides of reaction_lookup
 * derived their keys from different sources and silently never joined —
 * zero imessage_tapback DPO pairs from 2026-05-31 to 2026-07-19. */

/* `src` may alias `out` (in-place normalization is explicitly supported —
 * see the idempotency contract in the header), and the result is always a
 * SUFFIX of the source, so the regions overlap. memmove, not memcpy. */
static hu_error_t rxn_copy_out(const char *src, char *out, size_t cap) {
    if (!out || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!src) {
        out[0] = '\0';
        return HU_OK;
    }
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memmove(out, src, n);
    out[n] = '\0';
    return HU_OK;
}

hu_error_t hu_imessage_strip_assoc_guid_prefix(const char *raw, char *out, size_t cap) {
    if (!out || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    /* Consume `raw` BEFORE writing `out` — they may be the same buffer, and
     * a defensive out[0]='\0' here would destroy the input. */
    if (!raw || !raw[0]) {
        out[0] = '\0';
        return HU_OK;
    }

    /* Match "p:<digits>/" exactly — one or more digits, then a slash. Anything
     * else (including a bare GUID, or a GUID that merely contains a slash) is
     * passed through untouched. */
    const char *p = raw;
    if (p[0] == 'p' && p[1] == ':') {
        const char *d = p + 2;
        while (isdigit((unsigned char)*d))
            d++;
        if (d > p + 2 && *d == '/')
            return rxn_copy_out(d + 1, out, cap);
    }
    return rxn_copy_out(raw, out, cap);
}

hu_error_t hu_imessage_normalize_thread_key(const char *raw, char *out, size_t cap) {
    if (!out || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    /* Consume `raw` BEFORE writing `out` — see the aliasing note above. */
    if (!raw || !raw[0]) {
        out[0] = '\0';
        return HU_OK;
    }

    /* Everything after the LAST ';' is the bare conversation id:
     *   "any;-;+15551234567"      -> "+15551234567"   (DM, phone)
     *   "any;-;seth@me.com"       -> "seth@me.com"    (DM, email)
     *   "any;+;chat9755112348937" -> "chat9755112348937" (group)
     *   "+15551234567"            -> "+15551234567"   (already bare)
     *
     * strrchr rather than counting two fields: robust to any prefix count or
     * format Apple ships next, and trivially idempotent because a bare id
     * contains no ';'.
     *
     * Consequence, deliberate: on older macOS emitting "SMS;-;X" and
     * "iMessage;-;X", both reduce to X — one training key per human rather
     * than one per transport. For DPO that is the intent (same person, same
     * voice); this machine only emits the "any;" service anyway. */
    const char *last = strrchr(raw, ';');
    return rxn_copy_out(last ? last + 1 : raw, out, cap);
}

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

hu_error_t hu_imessage_poll_reactions(const char *db_path, int64_t since_unix,
                                      hu_reaction_event_t *out, size_t cap, size_t *out_n) {
    if (!db_path || !out || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;
#if HU_IS_TEST || !defined(__APPLE__) || !defined(__MACH__) || !defined(HU_ENABLE_SQLITE)
    (void)since_unix;
    (void)cap;
    return HU_ERR_NOT_SUPPORTED;
#else
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }
    /* iMessage reactions: associated_message_type 2000-2006 (add) or
     * 3000-3006 (remove) with associated_message_guid pointing at the
     * original message. The outer parens around the OR are required for
     * SQL precedence — without them, the AND below would bind tighter
     * and silently drop the 3xxx removal rows. */
    /* Phase 2 of docs/plans/2026-05-18-imessage-sota.md: also pull
     * associated_message_emoji (iOS 17+) so CUSTOM_EMOJI tapbacks
     * carry the actual glyph through to the personal-model sink.
     * The column may not exist on Big Sur / Monterey; we probe via
     * a SELECT-with-fallback pattern: try the new column first, on
     * SQLITE_ERROR retry without it. */
    const char *sql_v17 = "SELECT m.associated_message_type, m.associated_message_guid, "
                          "       m.handle_id, h.id, c.guid, m.date, "
                          "       m.associated_message_emoji "
                          "FROM message m "
                          "JOIN chat_message_join cmj ON cmj.message_id = m.ROWID "
                          "JOIN chat c ON c.ROWID = cmj.chat_id "
                          "LEFT JOIN handle h ON h.ROWID = m.handle_id "
                          "WHERE (m.associated_message_type BETWEEN 2000 AND 2006 OR "
                          "m.associated_message_type BETWEEN 3000 AND 3006) "
                          "  AND m.associated_message_guid IS NOT NULL "
                          "  AND m.date > ((? - 978307200) * 1000000000) "
                          "ORDER BY m.date DESC LIMIT ?";
    const char *sql_legacy = "SELECT m.associated_message_type, m.associated_message_guid, "
                             "       m.handle_id, h.id, c.guid, m.date "
                             "FROM message m "
                             "JOIN chat_message_join cmj ON cmj.message_id = m.ROWID "
                             "JOIN chat c ON c.ROWID = cmj.chat_id "
                             "LEFT JOIN handle h ON h.ROWID = m.handle_id "
                             "WHERE (m.associated_message_type BETWEEN 2000 AND 2006 OR "
                             "m.associated_message_type BETWEEN 3000 AND 3006) "
                             "  AND m.associated_message_guid IS NOT NULL "
                             "  AND m.date > ((? - 978307200) * 1000000000) "
                             "ORDER BY m.date DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    bool emoji_available = true;
    if (sqlite3_prepare_v2(db, sql_v17, -1, &stmt, NULL) != SQLITE_OK) {
        /* Older chat.db schemas (pre-iOS 17 / Ventura) lack
         * associated_message_emoji. Fall back to the legacy SQL so the
         * reader still works on Big Sur / Monterey. */
        emoji_available = false;
        if (sqlite3_prepare_v2(db, sql_legacy, -1, &stmt, NULL) != SQLITE_OK) {
            sqlite3_close(db);
            return HU_ERR_IO;
        }
    }
    sqlite3_bind_int64(stmt, 1, since_unix);
    sqlite3_bind_int(stmt, 2, (int)cap);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_n < cap) {
        int code = sqlite3_column_int(stmt, 0);
        const unsigned char *guid = sqlite3_column_text(stmt, 1);
        const unsigned char *handle = sqlite3_column_text(stmt, 3);
        const unsigned char *chat_guid = sqlite3_column_text(stmt, 4);
        int64_t mac_ns = sqlite3_column_int64(stmt, 5);
        const unsigned char *emoji = emoji_available ? sqlite3_column_text(stmt, 6) : NULL;

        hu_reaction_kind_t k = HU_REACTION_UNKNOWN;
        hu_reaction_polarity_t p = HU_REACTION_NEUTRAL;
        if (hu_reaction_normalize_imessage(code, &k, &p) != HU_OK)
            continue;

        /* Normalize BOTH join-key fields before they leave this channel. The
         * reaction_lookup store is an exact-match join against keys the daemon
         * reply router registered; chat.db spells the same conversation
         * ("any;-;+1555" vs "+1555") and the same message ("p:0/GUID" vs
         * "GUID") differently, so un-normalized keys never match. */
        char thread_key[256];
        char msg_key[128];
        (void)hu_imessage_normalize_thread_key(chat_guid ? (const char *)chat_guid : NULL,
                                               thread_key, sizeof(thread_key));
        (void)hu_imessage_strip_assoc_guid_prefix(guid ? (const char *)guid : NULL, msg_key,
                                                  sizeof(msg_key));

        out[*out_n].channel_id = "imessage";
        out[*out_n].target_thread_id = thread_key[0] ? strdup(thread_key) : NULL;
        out[*out_n].target_message_ref = msg_key[0] ? strdup(msg_key) : NULL;
        out[*out_n].sender_handle = handle ? strdup((const char *)handle) : NULL;
        out[*out_n].kind = k;
        out[*out_n].polarity = p;
        out[*out_n].timestamp_unix = (mac_ns / 1000000000) + 978307200;
        out[*out_n].is_removal = code >= 3000 ? 1 : 0;
        out[*out_n].emoji = (emoji && emoji[0]) ? strdup((const char *)emoji) : NULL;
        (*out_n)++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return HU_OK;
#endif
}

hu_error_t hu_imessage_lookup_latest_sent_guid(const char *db_path, const char *chat_guid,
                                               const char *text_prefix, char *out_guid,
                                               size_t out_cap) {
    if (!db_path || !chat_guid || !out_guid || out_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    out_guid[0] = '\0';
#if HU_IS_TEST || !defined(__APPLE__) || !defined(__MACH__) || !defined(HU_ENABLE_SQLITE)
    (void)text_prefix;
    return HU_ERR_NOT_SUPPORTED;
#else
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }
    /* `chat_guid` may arrive as a full chat.guid ("any;-;+1555") or as the
     * bare handle ("+1555") — the daemon reply router carries the bare form.
     * Match either: exact, or c.guid ending in ";" || <bare>. Suffix compare
     * via substr (NOT LIKE) so a handle containing '_' or '%' can't act as a
     * wildcard. Numbered params (?1/?2) let chat_guid bind once.
     *
     * Before this accepted the bare form, resolution ALWAYS missed and the
     * caller fell back to a synthetic "out-<ts>" msg_ref, which could never
     * match a real tapback GUID — zero imessage_tapback DPO pairs. */
    const char *sql = "SELECT m.guid FROM message m "
                      "JOIN chat_message_join cmj ON cmj.message_id = m.ROWID "
                      "JOIN chat c ON c.ROWID = cmj.chat_id "
                      "WHERE (c.guid = ?1 OR (length(c.guid) > length(?1) "
                      "       AND substr(c.guid, length(c.guid) - length(?1)) = ';' || ?1)) "
                      "  AND m.is_from_me = 1 "
                      "  AND (?2 IS NULL OR m.text LIKE ?2 || '%') "
                      "ORDER BY m.date DESC LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_bind_text(stmt, 1, chat_guid, -1, SQLITE_STATIC);
    if (text_prefix && text_prefix[0])
        sqlite3_bind_text(stmt, 2, text_prefix, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 2);
    hu_error_t err = HU_ERR_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *g = sqlite3_column_text(stmt, 0);
        if (g) {
            size_t glen = strlen((const char *)g);
            if (glen + 1 <= out_cap) {
                memcpy(out_guid, g, glen + 1);
                err = HU_OK;
            } else {
                err = HU_ERR_INVALID_ARGUMENT;
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return err;
#endif
}
