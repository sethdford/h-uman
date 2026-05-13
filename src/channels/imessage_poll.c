/*
 * imessage_poll.c — chat.db reads: poll, history, attachments, GUID lookups,
 * user-activity probe.
 *
 * Step 5 of the iMessage shape refactor (last per
 * docs/plans/2026-05-12-imessage-shape-refactor.md — the highest-risk
 * single extraction, landed after every other module is already in its
 * own file).
 *
 * What lives here
 * ===============
 *
 * The chat.db reading surface. Every function in this file opens
 * ~/Library/Messages/chat.db (read-only via the imessage_open_chatdb
 * shared helper) and returns structured data to the channel runtime:
 *
 *   - hu_imessage_user_responded_recently — activity probe used by the
 *     daemon to suppress sends when the real user is active.
 *   - imessage_load_conversation_history  — vtable hook for history
 *     retrieval; reads attributedBody, classifies stickers / effects /
 *     balloon types.
 *   - hu_imessage_get_attachment_path / _latest_attachment_path —
 *     resolve attachment file paths by message id or contact.
 *   - hu_imessage_lookup_message_by_guid  — inline-reply context lookup.
 *   - hu_imessage_poll                    — main poll function (timer-
 *                                           based SQL when imsg watch
 *                                           isn't producing).
 *
 * Why last
 * ========
 *
 * Poll is the steady-state hot path. It interacts with chat.db schema
 * that has drifted across macOS releases (date_retracted, attributedBody,
 * balloon_bundle_id, expressive_send_style_id), and it carries the
 * circuit-breaker / poll-status bookkeeping. Extracting last — after
 * every other module is already separately reviewable — means any
 * regression here can be bisected with maximum precision.
 *
 * Behavior preserved
 * ==================
 *
 * This is a pure file-move. The chat.db SQL and parsing logic is
 * byte-identical to the pre-refactor `imessage.c`. The followups
 * called out by the audit (schema-cache, IMSG_POLL_SQL_BASE header)
 * are left as their own focused commits so this carve-out remains a
 * clean refactor.
 *
 * attributedBody null-blob convention
 * ====================================
 *
 * The audit's B2 item flagged a possible null-deref pattern around
 * `sqlite3_column_blob()` returning NULL even when `_column_bytes()`
 * is non-zero. Investigation: every blob read in this file uses the
 * canonical guard `if (ab && ab_len > 0)` BEFORE deref (see lines
 * ~226, ~498, ~863 — search for `sqlite3_column_blob`). The called
 * helper `hu_imessage_extract_attributed_body` itself checks
 * `!blob || blob_len < 4` at entry. Two layers of NULL safety.
 *
 * When adding a new column_blob site, follow the convention:
 *   const unsigned char *ab = sqlite3_column_blob(stmt, idx);
 *   int ab_len = sqlite3_column_bytes(stmt, idx);
 *   if (ab && ab_len > 0) { ... use ab ... }
 *
 * Do NOT trust `ab_len > 0` alone — sqlite3 column type promotion can
 * produce non-zero bytes with a NULL pointer for empty BLOBs.
 */

#include "imessage_internal.h"

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)

#include <sqlite3.h>
#include <unistd.h>

bool hu_imessage_user_responded_recently(void *channel_ctx, const char *handle, size_t handle_len,
                                         int within_seconds) {
    if (!handle || handle_len == 0 || within_seconds <= 0)
        return false;

    const char *home = getenv("HOME");
    if (!home)
        return false;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return false;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return false;

    /*
     * Check for is_from_me=1 messages to this handle within the time window.
     * macOS Messages stores dates as nanoseconds since 2001-01-01 (Core Data epoch).
     */
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)channel_ctx;
    if (c && c->loopback_handle && handle_len > 0 && strlen(c->loopback_handle) == handle_len &&
        memcmp(c->loopback_handle, handle, handle_len) == 0)
        return false;

    /* Exclude messages sent by the AI: only count is_from_me=1 rows whose
     * timestamp is after the last known AI send (+3s grace for clock skew).
     * If the AI never sent, last_ai_send_epoch is 0 and the filter is a no-op. */
    int64_t ai_cutoff = 0;
    if (c && c->last_ai_send_epoch > 0)
        ai_cutoff = c->last_ai_send_epoch + 3;

    const char *sql = "SELECT COUNT(*) FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 1 "
                      "AND m.date > ((?2 - 978307200) * 1000000000) "
                      "AND m.date > ((?3 - 978307200) * 1000000000)";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, handle, (int)handle_len, NULL);

    time_t cutoff = time(NULL) - within_seconds;
    sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);
    sqlite3_bind_int64(stmt, 3, ai_cutoff);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = sqlite3_column_int(stmt, 0) > 0;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return found;
}

#endif /* !HU_IS_TEST && __APPLE__ && __MACH__ && HU_ENABLE_SQLITE */

/* ── imessage_load_conversation_history (vtable hook) ──────────────────
 *
 * The function compiles unconditionally (the vtable in imessage.c needs
 * to point at it on every build); the SQLite implementation is gated
 * internally and other builds return HU_ERR_NOT_SUPPORTED. */

hu_error_t imessage_load_conversation_history(void *ctx, hu_allocator_t *alloc,
                                              const char *contact_id, size_t contact_id_len,
                                              size_t limit, hu_channel_history_entry_t **out,
                                              size_t *out_count) {
    (void)ctx;
    if (!alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_SUPPORTED;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return HU_ERR_INTERNAL;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_INTERNAL;

    const char *sql = "SELECT m.is_from_me, m.text, "
                      "  datetime(m.date/1000000000 + 978307200, 'unixepoch', 'localtime') as ts, "
                      "  (SELECT COUNT(*) FROM message_attachment_join maj "
                      "   JOIN attachment a ON maj.attachment_id = a.ROWID "
                      "   WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "
                      "   AND (LOWER(a.filename) LIKE '%.mov' OR LOWER(a.filename) LIKE '%.mp4' "
                      "     OR LOWER(a.filename) LIKE '%.m4v')) > 0 AS has_video, "
                      "  (SELECT COUNT(*) FROM message_attachment_join maj2 "
                      "   JOIN attachment a2 ON maj2.attachment_id = a2.ROWID "
                      "   WHERE maj2.message_id = m.ROWID AND a2.filename IS NOT NULL "
                      "   AND (LOWER(a2.filename) LIKE '%.jpg' OR LOWER(a2.filename) LIKE '%.jpeg' "
                      "     OR LOWER(a2.filename) LIKE '%.png' OR LOWER(a2.filename) LIKE '%.heic' "
                      "     OR LOWER(a2.filename) LIKE '%.gif' OR LOWER(a2.filename) LIKE "
                      "'%.webp')) > 0 AS has_image, "
                      "  (SELECT COUNT(*) FROM message_attachment_join maj3 "
                      "   JOIN attachment a3 ON maj3.attachment_id = a3.ROWID "
                      "   WHERE maj3.message_id = m.ROWID AND a3.filename IS NOT NULL "
                      "   AND (LOWER(a3.filename) LIKE '%.caf' OR LOWER(a3.filename) LIKE '%.m4a' "
                      "     OR LOWER(a3.filename) LIKE '%.mp3' OR LOWER(a3.filename) LIKE '%.aac' "
                      "     OR LOWER(a3.filename) LIKE '%.opus')) > 0 AS has_audio, "
                      "  m.attributedBody, "
                      "  m.balloon_bundle_id, "
                      "  m.expressive_send_style_id "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.associated_message_type = 0 "
                      "ORDER BY m.date DESC LIMIT ?2";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_INTERNAL;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, -1, NULL);
    sqlite3_bind_int(stmt, 2, (int)(limit > 50 ? 50 : limit));

    if (limit > 50)
        limit = 50;
    hu_channel_history_entry_t *entries =
        (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(*entries));
    if (!entries) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, limit * sizeof(*entries));
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
        entries[count].from_me = sqlite3_column_int(stmt, 0) != 0;
        const char *txt = (const char *)sqlite3_column_text(stmt, 1);
        const char *ts = (const char *)sqlite3_column_text(stmt, 2);
        int has_video = sqlite3_column_int(stmt, 3);
        int has_image = sqlite3_column_int(stmt, 4);
        int has_audio = sqlite3_column_int(stmt, 5);

        /* macOS 15+: extract from attributedBody when text column is NULL */
        char attr_buf[4096];
        if ((!txt || txt[0] == '\0')) {
            const unsigned char *ab = sqlite3_column_blob(stmt, 6);
            int ab_len = sqlite3_column_bytes(stmt, 6);
            if (ab && ab_len > 0) {
                size_t extracted = hu_imessage_extract_attributed_body(ab, (size_t)ab_len, attr_buf,
                                                                       sizeof(attr_buf));
                if (extracted > 0)
                    txt = attr_buf;
            }
        }
        /* Sticker/Memoji classification from balloon_bundle_id (col 7) */
        const char *hist_balloon = (const char *)sqlite3_column_text(stmt, 7);
        if (hu_imessage_text_is_placeholder(txt)) {
            const char *label = hu_imessage_balloon_label(hist_balloon);
            if (label)
                txt = label;
        }
        /* Effect decoration from expressive_send_style_id (col 8) */
        const char *hist_effect = (const char *)sqlite3_column_text(stmt, 8);
        char hist_effect_buf[4200];
        if (txt && txt[0] != '\0') {
            const char *ename = hu_imessage_effect_name(hist_effect);
            if (ename) {
                snprintf(hist_effect_buf, sizeof(hist_effect_buf), "[Sent with %s] %s", ename, txt);
                txt = hist_effect_buf;
            }
        }
        if (txt && strlen(txt) > 0) {
            size_t tlen = strlen(txt);
            if (tlen >= sizeof(entries[0].text))
                tlen = sizeof(entries[0].text) - 1;
            memcpy(entries[count].text, txt, tlen);
            entries[count].text[tlen] = '\0';
        } else if (entries[count].from_me) {
            snprintf(entries[count].text, sizeof(entries[0].text), "[you replied]");
        } else {
            if (has_audio)
                snprintf(entries[count].text, sizeof(entries[0].text), "[Voice Message]");
            else if (has_video)
                snprintf(entries[count].text, sizeof(entries[0].text), "[Video]");
            else if (has_image)
                snprintf(entries[count].text, sizeof(entries[0].text), "[Photo]");
            else
                snprintf(entries[count].text, sizeof(entries[0].text), "[image or attachment]");
        }
        if (ts) {
            size_t tslen = strlen(ts);
            if (tslen >= sizeof(entries[0].timestamp))
                tslen = sizeof(entries[0].timestamp) - 1;
            memcpy(entries[count].timestamp, ts, tslen);
            entries[count].timestamp[tslen] = '\0';
        }
        count++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Results come in DESC order; reverse to chronological for caller */
    for (size_t i = 0; i < count / 2; i++) {
        hu_channel_history_entry_t tmp = entries[i];
        entries[i] = entries[count - 1 - i];
        entries[count - 1 - i] = tmp;
    }

    *out = entries;
    *out_count = count;
    return HU_OK;
#else
    (void)contact_id_len;
    (void)limit;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* ── Attachment paths ────────────────────────────────────────────────── */
#ifndef HU_IS_TEST
#if defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
char *hu_imessage_get_attachment_path(hu_allocator_t *alloc, int64_t message_id) {
    if (!alloc || message_id <= 0)
        return NULL;

    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return NULL;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return NULL;

    const char *sql = "SELECT a.filename FROM attachment a "
                      "JOIN message_attachment_join maj ON maj.attachment_id = a.ROWID "
                      "WHERE maj.message_id = ?1 LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_bind_int64(stmt, 1, message_id);

    char *path = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *filename = (const char *)sqlite3_column_text(stmt, 0);
        if (filename && filename[0]) {
            size_t len = strlen(filename);
            /* Expand ~ to home directory */
            if (len >= 1 && filename[0] == '~' && (len == 1 || filename[1] == '/')) {
                size_t home_len = strlen(home);
                size_t suffix_len = (len > 1) ? len - 1 : 0;
                size_t total = home_len + suffix_len + 1;
                path = (char *)alloc->alloc(alloc->ctx, total);
                if (path) {
                    memcpy(path, home, home_len);
                    if (suffix_len > 0)
                        memcpy(path + home_len, filename + 1, suffix_len);
                    path[total - 1] = '\0';
                }
            } else {
                path = hu_strndup(alloc, filename, len);
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    /* Validate path is within Messages attachments directory */
    if (path && home) {
        char allowed_prefix[512];
        int prefix_len = snprintf(allowed_prefix, sizeof(allowed_prefix),
                                  "%s/Library/Messages/Attachments/", home);
        if (prefix_len > 0 && (size_t)prefix_len < sizeof(allowed_prefix)) {
            if (strncmp(path, allowed_prefix, (size_t)prefix_len) != 0) {
                alloc->free(alloc->ctx, path, strlen(path) + 1);
                return NULL; /* Path outside allowed directory */
            }
        }
    }
    return path;
}
#else
char *hu_imessage_get_attachment_path(hu_allocator_t *alloc, int64_t message_id) {
    (void)alloc;
    (void)message_id;
    return NULL;
}
#endif

#if defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
char *hu_imessage_get_latest_attachment_path(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len) {
    if (!alloc || !contact_id || contact_id_len == 0)
        return NULL;

    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return NULL;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return NULL;

    /* Get the most recent message with attachment from this contact */
    const char *sql = "SELECT a.filename FROM attachment a "
                      "JOIN message_attachment_join maj ON maj.attachment_id = a.ROWID "
                      "JOIN message m ON maj.message_id = m.ROWID "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 0 "
                      "ORDER BY m.date DESC LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    char contact_buf[256];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, -1, NULL);

    char *path = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *filename = (const char *)sqlite3_column_text(stmt, 0);
        if (filename && filename[0]) {
            size_t len = strlen(filename);
            if (len >= 1 && filename[0] == '~' && (len == 1 || filename[1] == '/')) {
                size_t home_len = strlen(home);
                size_t suffix_len = (len > 1) ? len - 1 : 0;
                size_t total = home_len + suffix_len + 1;
                path = (char *)alloc->alloc(alloc->ctx, total);
                if (path) {
                    memcpy(path, home, home_len);
                    if (suffix_len > 0)
                        memcpy(path + home_len, filename + 1, suffix_len);
                    path[total - 1] = '\0';
                }
            } else {
                path = hu_strndup(alloc, filename, len);
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    /* Validate path is within Messages attachments directory */
    if (path && home) {
        char allowed_prefix[512];
        int prefix_len = snprintf(allowed_prefix, sizeof(allowed_prefix),
                                  "%s/Library/Messages/Attachments/", home);
        if (prefix_len > 0 && (size_t)prefix_len < sizeof(allowed_prefix)) {
            if (strncmp(path, allowed_prefix, (size_t)prefix_len) != 0) {
                alloc->free(alloc->ctx, path, strlen(path) + 1);
                return NULL; /* Path outside allowed directory */
            }
        }
    }
    return path;
}
#else
char *hu_imessage_get_latest_attachment_path(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len) {
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    return NULL;
}
#endif
#endif

/* ── Inline reply context: look up original message text by GUID ────── */

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
hu_error_t hu_imessage_lookup_message_by_guid(hu_allocator_t *alloc, const char *guid,
                                              size_t guid_len, char *out_text, size_t out_cap,
                                              size_t *out_len) {
    if (!alloc || !guid || guid_len == 0 || !out_text || out_cap == 0 || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = 0;
    out_text[0] = '\0';

    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_SUPPORTED;
    char db_path[512];
    if (snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home) < 0)
        return HU_ERR_INTERNAL;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_IO;

    const char *sql =
        "SELECT m.text, m.attributedBody, m.balloon_bundle_id, m.expressive_send_style_id "
        "FROM message m WHERE m.guid = ? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, guid, (int)guid_len, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        if (text && text[0] != '\0') {
            *out_len = hu_imessage_copy_bounded(out_text, out_cap, text, strlen(text));
        } else {
            const unsigned char *ab = sqlite3_column_blob(stmt, 1);
            int ab_len = sqlite3_column_bytes(stmt, 1);
            if (ab && ab_len > 0) {
                *out_len =
                    hu_imessage_extract_attributed_body(ab, (size_t)ab_len, out_text, out_cap);
            }
        }
        /* Sticker/Memoji fallback when text is empty or a generic placeholder */
        if (hu_imessage_text_is_placeholder(out_text)) {
            const char *bid = (const char *)sqlite3_column_text(stmt, 2);
            const char *label = hu_imessage_balloon_label(bid);
            if (label)
                *out_len = hu_imessage_copy_bounded(out_text, out_cap, label, strlen(label));
        }
        /* Effect prefix when text is present */
        if (out_text[0] != '\0') {
            const char *eid = (const char *)sqlite3_column_text(stmt, 3);
            const char *ename = hu_imessage_effect_name(eid);
            if (ename) {
                char tmp[4200];
                int n2 = snprintf(tmp, sizeof(tmp), "[Sent with %s] %s", ename, out_text);
                if (n2 > 0) {
                    size_t avail = (size_t)n2 < sizeof(tmp) ? (size_t)n2 : sizeof(tmp) - 1;
                    *out_len = hu_imessage_copy_bounded(out_text, out_cap, tmp, avail);
                }
            }
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return HU_OK;
}
#elif HU_IS_TEST

static struct {
    char guid[96];
    char text[512];
} s_test_guid_store[16];
static size_t s_test_guid_count;

void hu_imessage_test_set_guid_lookup(const char *guid, const char *text) {
    if (!guid || !text || s_test_guid_count >= 16)
        return;
    size_t i = s_test_guid_count++;
    size_t gl = strlen(guid);
    if (gl > 95)
        gl = 95;
    memcpy(s_test_guid_store[i].guid, guid, gl);
    s_test_guid_store[i].guid[gl] = '\0';
    size_t tl = strlen(text);
    if (tl > 511)
        tl = 511;
    memcpy(s_test_guid_store[i].text, text, tl);
    s_test_guid_store[i].text[tl] = '\0';
}

void hu_imessage_test_clear_guid_lookups(void) {
    s_test_guid_count = 0;
}

hu_error_t hu_imessage_lookup_message_by_guid(hu_allocator_t *alloc, const char *guid,
                                              size_t guid_len, char *out_text, size_t out_cap,
                                              size_t *out_len) {
    (void)alloc;
    if (!guid || guid_len == 0 || !out_text || out_cap == 0 || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = 0;
    out_text[0] = '\0';
    for (size_t i = 0; i < s_test_guid_count; i++) {
        if (strlen(s_test_guid_store[i].guid) == guid_len &&
            memcmp(s_test_guid_store[i].guid, guid, guid_len) == 0) {
            *out_len = hu_imessage_copy_bounded(out_text, out_cap, s_test_guid_store[i].text,
                                                strlen(s_test_guid_store[i].text));
            return HU_OK;
        }
    }
    return HU_ERR_NOT_SUPPORTED;
}
#else
hu_error_t hu_imessage_lookup_message_by_guid(hu_allocator_t *alloc, const char *guid,
                                              size_t guid_len, char *out_text, size_t out_cap,
                                              size_t *out_len) {
    (void)alloc;
    (void)guid;
    (void)guid_len;
    if (out_text && out_cap > 0)
        out_text[0] = '\0';
    if (out_len)
        *out_len = 0;
    return HU_ERR_NOT_SUPPORTED;
}
#endif

/* ── iMessage polling via ~/Library/Messages/chat.db ──────────────────── */

hu_error_t hu_imessage_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count) {
    (void)alloc;
    if (!channel_ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if HU_IS_TEST
    {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)channel_ctx;
        /* Mirror the production heartbeat contract: a healthy idle watch
         * tick refreshes the success epoch. Tests use
         * `hu_imessage_test_set_watch_running` to drive this branch. */
        if (c->imsg_watch_running)
            imessage_record_poll_heartbeat(c, (int64_t)time(NULL));
        if (c->mock_count > 0) {
            size_t n = c->mock_count < max_msgs ? c->mock_count : max_msgs;
            for (size_t i = 0; i < n; i++) {
                memcpy(msgs[i].session_key, c->mock_msgs[i].session_key, 128);
                memcpy(msgs[i].content, c->mock_msgs[i].content, 4096);
                msgs[i].message_id = (int64_t)(i + 1);
                msgs[i].is_group = c->mock_msgs[i].is_group;
                msgs[i].has_attachment = c->mock_msgs[i].has_attachment;
                msgs[i].has_video = c->mock_msgs[i].has_video;
                msgs[i].was_edited = c->mock_msgs[i].was_edited;
                msgs[i].was_unsent = c->mock_msgs[i].was_unsent;
                msgs[i].timestamp_sec = c->mock_msgs[i].timestamp_sec;
                memcpy(msgs[i].guid, c->mock_msgs[i].guid, 96);
                memcpy(msgs[i].reply_to_guid, c->mock_msgs[i].reply_to_guid, 96);
                memcpy(msgs[i].chat_id, c->mock_msgs[i].chat_id, 128);
            }
            *out_count = n;
            c->mock_count = 0;
            return HU_OK;
        }
        return HU_OK;
    }
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)max_msgs;
    return HU_ERR_NOT_SUPPORTED;
#elif !defined(HU_ENABLE_SQLITE)
    (void)max_msgs;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)channel_ctx;

    /* Breaker tripped: short-circuit with a clean HU_OK / 0 messages so the
     * daemon's outer poll-loop does not log "[human] poll error" on every
     * tick. The breaker already emitted ONE explanatory error line; from
     * there on, the doctor + status file carry the truth. The breaker auto-
     * resets on the next successful chat.db open, so we still attempt the
     * occasional probe (every Nth tick) to detect FDA recovery. */
    if (c->circuit_breaker_tripped) {
        c->breaker_recovery_probe_counter++;
        /* ~30 ticks ≈ 30s at 1s poll cadence. Cheap enough; rare enough that
         * a still-revoked FDA does not regenerate log spam. */
        if (c->breaker_recovery_probe_counter < 30)
            return HU_OK;
        c->breaker_recovery_probe_counter = 0;
        /* Fall through and attempt one open; if it succeeds, breaker resets. */
    }

    /* When imsg watch is active, skip the SQL query if no new data arrived.
     * This avoids redundant queries while maintaining sub-second latency.
     * If the watch process died, attempt restart before falling back to SQL. */
    if (c->imsg_watch_running) {
        if (!imsg_watch_has_data(c)) {
            /* Two cases here:
             *   1. Read returned EAGAIN: watch is alive, just no new data.
             *      `imsg_watch_running` stays true. This idle tick IS a
             *      successful poll cycle — record the heartbeat so the
             *      doctor's stall threshold doesn't trip on quiet hours.
             *   2. Read returned EOF / error: `imsg_watch_has_data` already
             *      tore down the watch and cleared `imsg_watch_running`.
             *      Don't claim success; let the next poll restart it. */
            if (c->imsg_watch_running)
                imessage_record_poll_heartbeat(c, (int64_t)time(NULL));
            return HU_OK;
        }
    } else if (c->use_imsg_cli && imsg_cli_available(c)) {
        imsg_watch_start(c);
    }

    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_SUPPORTED;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return HU_ERR_INTERNAL;

    sqlite3 *db = NULL;
    int rc = imessage_open_chatdb(db_path, &db);
    if (rc != SQLITE_OK) {
        /* Log only when not yet circuit-broken; once tripped the breaker
         * already explained the situation in a single high-priority line. */
        if (!c->circuit_breaker_tripped)
            hu_log_error("imessage", NULL, "cannot open chat.db: error %d", rc);
        (void)imessage_record_open_result(c, rc, (int64_t)time(NULL));
        imessage_save_poll_status(c);
        return HU_ERR_IO;
    }

    /* Detect date_retracted column (macOS Ventura+) for unsend detection */
    bool has_date_retracted = false;
    {
        sqlite3_stmt *col_check = NULL;
        if (sqlite3_prepare_v2(db, "SELECT date_retracted FROM message LIMIT 0", -1, &col_check,
                               NULL) == SQLITE_OK) {
            has_date_retracted = true;
            sqlite3_finalize(col_check);
        }
    }

    /* If last_rowid was never seeded (e.g. FDA wasn't granted at startup),
     * seed it now to current max so we only pick up truly new messages. */
    if (c->last_rowid == 0) {
        sqlite3_stmt *seed = NULL;
        if (sqlite3_prepare_v2(db, "SELECT MAX(ROWID) FROM message", -1, &seed, NULL) ==
            SQLITE_OK) {
            if (sqlite3_step(seed) == SQLITE_ROW)
                c->last_rowid = sqlite3_column_int64(seed, 0);
            sqlite3_finalize(seed);
        }
        hu_log_info("imessage", NULL,
                    "late-seeded last_rowid=%lld (only new messages will be processed)",
                    (long long)c->last_rowid);
        sqlite3_close(db);
        imessage_record_poll_success(c, (int64_t)time(NULL));
        imessage_save_poll_status(c);
        *out_count = 0;
        return HU_OK;
    }

    /* Column layout (0-indexed):
     *   0: ROWID, 1: guid, 2: text (COALESCE), 3: handle.id,
     *   4: participant_count, 5: has_image, 6: has_video, 7: has_audio,
     *   8: was_edited, 9: thread_originator_guid, 10: attributedBody,
     *   11: balloon_bundle_id, 12: expressive_send_style_id, 13: unix_ts,
     *   14: was_retracted (only when has_date_retracted, else absent),
     *   15: chat_id (from chat_message_join → chat.guid)
     *
     * Attachment type classification uses EXISTS (cheaper than COUNT). */

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

#define IMSG_POLL_SQL_RETRACT ", CASE WHEN m.date_retracted > 0 THEN 1 ELSE 0 END AS was_retracted"

#define IMSG_POLL_SQL_CHAT_ID                       \
    ", (SELECT c.guid FROM chat_message_join cmj2 " \
    "   JOIN chat c ON cmj2.chat_id = c.ROWID "     \
    "   WHERE cmj2.message_id = m.ROWID LIMIT 1) AS chat_guid"

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

    /* Build SQL variant based on available columns */
    char sql_buf[4096];
    int sql_len;
    if (has_date_retracted) {
        sql_len = snprintf(sql_buf, sizeof(sql_buf), "%s%s%s%s", IMSG_POLL_SQL_BASE,
                           IMSG_POLL_SQL_RETRACT, IMSG_POLL_SQL_CHAT_ID, IMSG_POLL_SQL_FROM);
    } else {
        sql_len = snprintf(sql_buf, sizeof(sql_buf), "%s%s%s", IMSG_POLL_SQL_BASE,
                           IMSG_POLL_SQL_CHAT_ID, IMSG_POLL_SQL_FROM);
    }
    if (sql_len < 0 || (size_t)sql_len >= sizeof(sql_buf)) {
        sqlite3_close(db);
        return HU_ERR_INTERNAL;
    }

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql_buf, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        hu_log_error("imessage", NULL, "SQL prepare failed: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, c->last_rowid);
    sqlite3_bind_int(stmt, 2, (int)max_msgs);
    sqlite3_bind_text(stmt, 3, c->loopback_handle ? c->loopback_handle : "", -1, SQLITE_STATIC);

    const int col_retracted = has_date_retracted ? 14 : -1;
    const int col_chat_guid = has_date_retracted ? 15 : 14;

    size_t count = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_msgs) {
        int64_t rowid = sqlite3_column_int64(stmt, 0);
        const char *guid = (const char *)sqlite3_column_text(stmt, 1);
        const char *text = (const char *)sqlite3_column_text(stmt, 2);
        const char *handle = (const char *)sqlite3_column_text(stmt, 3);
        int participant_count = sqlite3_column_int(stmt, 4);
        int has_image = sqlite3_column_int(stmt, 5);
        int has_video = sqlite3_column_int(stmt, 6);
        int has_audio = sqlite3_column_int(stmt, 7);
        int was_edited = sqlite3_column_int(stmt, 8);
        const char *reply_to = (const char *)sqlite3_column_text(stmt, 9);

        /* macOS 15+: text column is often NULL while attributedBody has the
         * actual content. The COALESCE in the query substitutes '[Photo]' for
         * NULL text, so also try attributedBody when text is a placeholder. */
        char attr_text_buf[4096];
        if (!text || text[0] == '\0' || hu_imessage_text_is_placeholder(text)) {
            const unsigned char *attr_blob = sqlite3_column_blob(stmt, 10);
            int attr_len = sqlite3_column_bytes(stmt, 10);
            if (attr_blob && attr_len > 0) {
                size_t extracted = hu_imessage_extract_attributed_body(
                    attr_blob, (size_t)attr_len, attr_text_buf, sizeof(attr_text_buf));
                if (extracted > 0)
                    text = attr_text_buf;
            }
        }

        /* Sticker/Memoji detection via balloon_bundle_id (col 11).
         * Override text only when it's empty OR a COALESCE-generated generic label,
         * because Memoji/Sticker messages have no real text but COALESCE may set '[Photo]'. */
        const char *balloon_id = (const char *)sqlite3_column_text(stmt, 11);
        if (balloon_id && balloon_id[0]) {
            const char *label = hu_imessage_balloon_label(balloon_id);
            if (label && hu_imessage_text_is_placeholder(text))
                text = label;
        }

        /* Message effect detection via expressive_send_style_id (col 12) */
        const char *effect_id = (const char *)sqlite3_column_text(stmt, 12);
        char effect_buf[4200];
        if (text && text[0]) {
            const char *effect_name = hu_imessage_effect_name(effect_id);
            if (effect_name) {
                snprintf(effect_buf, sizeof(effect_buf), "[Sent with %s] %s", effect_name, text);
                text = effect_buf;
            }
        }

        int64_t msg_unix_ts = sqlite3_column_int64(stmt, 13);

        if (!text || !handle) {
            c->last_rowid = rowid;
            continue;
        }

        /* Skip messages that match content we recently sent (echo prevention) */
        if (imessage_was_sent_by_us(c, text, strlen(text))) {
            c->last_rowid = rowid;
            continue;
        }

        size_t handle_len = strlen(handle);
        if (c->allow_from_count > 0) {
            bool allowed = false;
            for (size_t i = 0; i < c->allow_from_count; i++) {
                const char *a = c->allow_from[i];
                if (!a)
                    continue;
                if (a[0] == '*' && a[1] == '\0') {
                    allowed = true;
                    break;
                }
                size_t a_len = strlen(a);
                if (a_len == handle_len && strncasecmp(handle, a, a_len) == 0) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                c->last_rowid = rowid;
                continue;
            }
        }
        size_t text_len = strlen(text);
        if (handle_len >= sizeof(msgs[count].session_key))
            handle_len = sizeof(msgs[count].session_key) - 1;
        if (text_len >= sizeof(msgs[count].content))
            text_len = sizeof(msgs[count].content) - 1;

        memcpy(msgs[count].session_key, handle, handle_len);
        msgs[count].session_key[handle_len] = '\0';
        memcpy(msgs[count].content, text, text_len);
        msgs[count].content[text_len] = '\0';
        msgs[count].message_id = rowid;
        msgs[count].is_group = (participant_count > 2);
        msgs[count].has_attachment = (has_image != 0 || has_audio != 0);
        msgs[count].has_video = (has_video != 0);
        if (guid && strlen(guid) > 0) {
            size_t g_len = strlen(guid);
            if (g_len >= sizeof(msgs[count].guid))
                g_len = sizeof(msgs[count].guid) - 1;
            memcpy(msgs[count].guid, guid, g_len);
            msgs[count].guid[g_len] = '\0';
        } else {
            msgs[count].guid[0] = '\0';
        }
        msgs[count].was_edited = (was_edited != 0);
        msgs[count].was_unsent =
            (col_retracted >= 0) ? (sqlite3_column_int(stmt, col_retracted) != 0) : false;
        if (reply_to && reply_to[0]) {
            size_t rt_len = strlen(reply_to);
            if (rt_len >= sizeof(msgs[count].reply_to_guid))
                rt_len = sizeof(msgs[count].reply_to_guid) - 1;
            memcpy(msgs[count].reply_to_guid, reply_to, rt_len);
            msgs[count].reply_to_guid[rt_len] = '\0';
        } else {
            msgs[count].reply_to_guid[0] = '\0';
        }
        msgs[count].timestamp_sec = msg_unix_ts;

        const char *chat_guid = (const char *)sqlite3_column_text(stmt, col_chat_guid);
        if (chat_guid && chat_guid[0]) {
            size_t cg_len = strlen(chat_guid);
            if (cg_len >= sizeof(msgs[count].chat_id))
                cg_len = sizeof(msgs[count].chat_id) - 1;
            memcpy(msgs[count].chat_id, chat_guid, cg_len);
            msgs[count].chat_id[cg_len] = '\0';
        } else {
            msgs[count].chat_id[0] = '\0';
        }

        c->last_rowid = rowid;
        count++;
        if (getenv("HU_DEBUG"))
            hu_log_info("imessage", NULL, "incoming handle=%s len=%zu", handle, text_len);
    }

    if (step_rc != SQLITE_DONE && step_rc != SQLITE_ROW)
        hu_log_error("imessage", NULL, "poll step unexpected result: %d (%s)", step_rc,
                     sqlite3_errmsg(db));

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (count > 0)
        imessage_save_rowid(c->last_rowid);

    if (count == 0 && getenv("HU_DEBUG"))
        hu_log_info("imessage", NULL, "poll: 0 messages (last_rowid=%lld)",
                    (long long)c->last_rowid);

    imessage_record_poll_success(c, (int64_t)time(NULL));
    imessage_save_poll_status(c);

    *out_count = count;
    return HU_OK;
#endif
}
