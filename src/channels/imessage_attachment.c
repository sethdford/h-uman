/*
 * imessage_attachment.c — Attachment + tapback + read-receipt context + GIFs.
 *
 * Step 7 of the iMessage shape refactor — see
 * docs/plans/2026-05-12-imessage-shape-refactor.md.
 *
 * This file owns the chat.db-backed "what happened around our messages"
 * lookups that the prompt builder injects as context, plus the Tenor GIF
 * fetch path used when sending a reaction GIF.
 *
 * What lives here
 * ===============
 *   - hu_imessage_build_tapback_context — read tapbacks they sent on
 *     our recent messages (love / like / laugh / emphasis / question /
 *     custom-emoji counts), formatted as a single bracketed string.
 *   - hu_imessage_build_read_receipt_context — detect "read but no
 *     reply" / "delivered but not read" states and emit a tactful
 *     directive (don't guilt-trip).
 *   - hu_imessage_count_recent_gif_tapbacks /
 *     hu_imessage_count_recent_music_tapbacks — feedback signals for
 *     the GIF / music send decision heuristics.
 *   - hu_imessage_get_latest_sent_rowid — chat.db lookup used by the
 *     tapback-feedback path.
 *   - hu_imessage_fetch_gif — Tenor v2 API call + temp-file download.
 *     Pure JSON-extract + HTTP (no Apple-only deps), so it compiles
 *     wherever HU_HTTP_CURL is enabled.
 *   - gif_json_extract — internal fallback JSON-string extractor used
 *     when the structured hu_json parse fails (resilience to Tenor
 *     response-shape changes).
 *   - hu_imessage_test_gif_json_extract — test-only public wrapper.
 *
 * Each public function has two implementations: an Apple+SQLite (or
 * HU_HTTP_CURL for fetch_gif) build, and a no-op stub for other
 * platforms / minimal builds. Same shape, no behavior change.
 */

#include "imessage_internal.h"

#include "human/core/error.h"
#include "human/core/io_secure.h"
#include "human/core/log.h"
#include "human/core/string.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
#include <unistd.h>
#endif

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
hu_error_t hu_imessage_build_tapback_context(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len, char **out, size_t *out_len) {
    if (!alloc || !contact_id || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    char db_path[512];
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_IO;
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_IO;

    const char *sql =
        "SELECT m.associated_message_type, COUNT(*) "
        "FROM message m "
        "WHERE m.is_from_me = 0 "
        "  AND m.associated_message_type BETWEEN 2000 AND 2006 "
        "  AND m.associated_message_guid IN ("
        "    SELECT m2.guid FROM message m2 "
        "    WHERE m2.is_from_me = 1 "
        "    AND m2.handle_id = (SELECT ROWID FROM handle WHERE id = ?1) "
        "    ORDER BY m2.date DESC LIMIT 5"
        "  ) "
        "  AND m.date > (strftime('%s', 'now') - 86400) * 1000000000 - 978307200000000000 "
        "GROUP BY m.associated_message_type";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);

    int hearts = 0, likes = 0, dislikes = 0, laughs = 0, emphasis = 0, questions = 0;
    int custom_emoji = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int type = sqlite3_column_int(stmt, 0);
        int cnt = sqlite3_column_int(stmt, 1);
        switch (type) {
        case 2000:
            hearts = cnt;
            break;
        case 2001:
            likes = cnt;
            break;
        case 2002:
            dislikes = cnt;
            break;
        case 2003:
            laughs = cnt;
            break;
        case 2004:
            emphasis = cnt;
            break;
        case 2005:
            questions = cnt;
            break;
        case 2006:
            custom_emoji = cnt;
            break;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    int total = hearts + likes + dislikes + laughs + emphasis + questions + custom_emoji;
    if (total == 0)
        return HU_OK;

    char buf[256];
    size_t pos = 0;
    pos = hu_buf_appendf(buf, sizeof(buf), pos, "[REACTIONS on your recent messages:");
    if (hearts > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d heart%s", hearts, hearts > 1 ? "s" : "");
    if (likes > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d like%s", likes, likes > 1 ? "s" : "");
    if (laughs > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d laugh%s", laughs, laughs > 1 ? "s" : "");
    if (emphasis > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d emphasis", emphasis);
    if (questions > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d question%s", questions,
                             questions > 1 ? "s" : "");
    if (dislikes > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d dislike%s", dislikes,
                             dislikes > 1 ? "s" : "");
    if (custom_emoji > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d emoji reaction%s", custom_emoji,
                             custom_emoji > 1 ? "s" : "");
    pos = hu_buf_appendf(buf, sizeof(buf), pos, "]");

    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

int hu_imessage_count_recent_gif_tapbacks(const char *contact_id, size_t contact_id_len) {
    if (!contact_id || contact_id_len == 0)
        return 0;

    const char *home = getenv("HOME");
    if (!home)
        return 0;

    char db_path[512];
    int dp = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (dp < 0 || (size_t)dp >= sizeof(db_path))
        return 0;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return 0;

    /* Find positive tapbacks (love/like/laugh/emphasis/emoji = 2000-2004,2006) on our
     * messages that have GIF attachments, from this contact, in the last 24 hours. */
    const char *sql =
        "SELECT COUNT(*) FROM message m "
        "WHERE m.is_from_me = 0 "
        "  AND (m.associated_message_type BETWEEN 2000 AND 2004 "
        "       OR m.associated_message_type = 2006) "
        "  AND m.handle_id = (SELECT ROWID FROM handle WHERE id = ?1) "
        "  AND m.date > (strftime('%s', 'now') - 86400) * 1000000000 - 978307200000000000 "
        "  AND m.associated_message_guid IN ("
        "    SELECT m2.guid FROM message m2 "
        "    JOIN message_attachment_join maj ON maj.message_id = m2.ROWID "
        "    JOIN attachment a ON maj.attachment_id = a.ROWID "
        "    WHERE m2.is_from_me = 1 "
        "      AND LOWER(a.filename) LIKE '%.gif' "
        "      AND m2.date > (strftime('%s', 'now') - 86400) * 1000000000 - 978307200000000000"
        "  )";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);

    int result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

int hu_imessage_count_recent_music_tapbacks(const char *contact_id, size_t contact_id_len) {
    if (!contact_id || contact_id_len == 0)
        return 0;

    const char *home = getenv("HOME");
    if (!home)
        return 0;

    char db_path[512];
    int dp = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (dp < 0 || (size_t)dp >= sizeof(db_path))
        return 0;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return 0;

    const char *sql =
        "SELECT COUNT(*) FROM message m "
        "WHERE m.is_from_me = 0 "
        "  AND (m.associated_message_type BETWEEN 2000 AND 2004 "
        "       OR m.associated_message_type = 2006) "
        "  AND m.handle_id = (SELECT ROWID FROM handle WHERE id = ?1) "
        "  AND m.date > (strftime('%s', 'now') - 86400) * 1000000000 - 978307200000000000 "
        "  AND m.associated_message_guid IN ("
        "    SELECT m2.guid FROM message m2 "
        "    JOIN message_attachment_join maj ON maj.message_id = m2.ROWID "
        "    JOIN attachment a ON maj.attachment_id = a.ROWID "
        "    WHERE m2.is_from_me = 1 "
        "      AND LOWER(a.filename) LIKE '%.m4a' "
        "      AND m2.date > (strftime('%s', 'now') - 86400) * 1000000000 - 978307200000000000"
        "  )";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);

    int cnt = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        cnt = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return cnt;
}

int64_t hu_imessage_get_latest_sent_rowid(const char *handle, size_t handle_len) {
    if (!handle || handle_len == 0)
        return -1;

    const char *home = getenv("HOME");
    if (!home)
        return -1;

    char db_path[512];
    int dp = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (dp < 0 || (size_t)dp >= sizeof(db_path))
        return -1;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return -1;

    const char *sql = "SELECT MAX(m.ROWID) FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE m.is_from_me = 1 AND h.id = ?1";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    char hbuf[128];
    size_t hlen = handle_len < sizeof(hbuf) - 1 ? handle_len : sizeof(hbuf) - 1;
    memcpy(hbuf, handle, hlen);
    hbuf[hlen] = '\0';
    sqlite3_bind_text(stmt, 1, hbuf, (int)hlen, SQLITE_STATIC);

    int64_t rowid = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        rowid = sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rowid;
}

hu_error_t hu_imessage_build_read_receipt_context(hu_allocator_t *alloc, const char *contact_id,
                                                  size_t contact_id_len, char **out,
                                                  size_t *out_len) {
    if (!alloc || !contact_id || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    char db_path[512];
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_IO;
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_IO;

    /* Find our last sent message to this contact and check its read status */
    const char *sql = "SELECT m.date, m.date_delivered, m.date_read, m.text "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 1 "
                      "ORDER BY m.date DESC LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);

    char buf[256];
    buf[0] = '\0';
    size_t pos = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t sent_date = sqlite3_column_int64(stmt, 0);
        int64_t delivered = sqlite3_column_int64(stmt, 1);
        int64_t read_date = sqlite3_column_int64(stmt, 2);

        /* Convert Apple epoch (nanoseconds since 2001-01-01) to Unix epoch */
        int64_t apple_epoch = 978307200LL;
        int64_t sent_unix = apple_epoch + sent_date / 1000000000LL;
        int64_t now_unix = (int64_t)time(NULL);
        int64_t age_seconds = now_unix - sent_unix;

        /* Also check: has there been a reply from them after our message? */
        sqlite3_stmt *reply_stmt = NULL;
        const char *reply_sql = "SELECT COUNT(*) FROM message m "
                                "JOIN handle h ON m.handle_id = h.ROWID "
                                "WHERE h.id = ?1 AND m.is_from_me = 0 AND m.date > ?2 "
                                "AND m.associated_message_type = 0";
        bool has_reply = false;
        if (sqlite3_prepare_v2(db, reply_sql, -1, &reply_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(reply_stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);
            sqlite3_bind_int64(reply_stmt, 2, sent_date);
            if (sqlite3_step(reply_stmt) == SQLITE_ROW)
                has_reply = sqlite3_column_int(reply_stmt, 0) > 0;
            sqlite3_finalize(reply_stmt);
        }

        if (!has_reply && read_date > 0 && age_seconds > 300 && age_seconds < 86400) {
            /* Read but no reply — they saw it but haven't responded */
            int64_t read_unix = apple_epoch + read_date / 1000000000LL;
            int64_t since_read = now_unix - read_unix;
            if (since_read > 60) {
                int mins = (int)(since_read / 60);
                if (mins > 60) {
                    pos = (size_t)snprintf(
                        buf, sizeof(buf),
                        "[READ RECEIPT: They read your last message %dh ago but haven't replied. "
                        "Don't mention this — just be natural, don't guilt-trip.]",
                        mins / 60);
                } else {
                    pos =
                        (size_t)snprintf(buf, sizeof(buf),
                                         "[READ RECEIPT: They read your last message %d min ago "
                                         "but haven't replied. "
                                         "Don't mention this — just be natural, don't guilt-trip.]",
                                         mins);
                }
            }
        } else if (!has_reply && delivered > 0 && read_date == 0 && age_seconds > 600) {
            /* Delivered but not read */
            pos = (size_t)snprintf(buf, sizeof(buf),
                                   "[READ RECEIPT: Your last message was delivered but not yet "
                                   "read. They may be busy.]");
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (pos > 0 && pos < sizeof(buf)) {
        *out = hu_strndup(alloc, buf, pos);
        if (!*out)
            return HU_ERR_OUT_OF_MEMORY;
        *out_len = pos;
    }
    return HU_OK;
}
#else
hu_error_t hu_imessage_build_tapback_context(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len, char **out, size_t *out_len) {
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_OK;
}

int hu_imessage_count_recent_gif_tapbacks(const char *contact_id, size_t contact_id_len) {
    (void)contact_id;
    (void)contact_id_len;
    return 0;
}

int hu_imessage_count_recent_music_tapbacks(const char *contact_id, size_t contact_id_len) {
    (void)contact_id;
    (void)contact_id_len;
    return 0;
}

int64_t hu_imessage_get_latest_sent_rowid(const char *handle, size_t handle_len) {
    (void)handle;
    (void)handle_len;
    return -1;
}

hu_error_t hu_imessage_build_read_receipt_context(hu_allocator_t *alloc, const char *contact_id,
                                                  size_t contact_id_len, char **out,
                                                  size_t *out_len) {
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_OK;
}
#endif

#if HU_IS_TEST || defined(HU_HTTP_CURL)
/* Simple JSON string extractor: find "key":"value" and return value.
 * Writes into out (up to cap). Returns length or 0 on failure. */
static size_t gif_json_extract(const char *json, size_t json_len, const char *key, char *out,
                               size_t cap) {
    size_t key_len = strlen(key);
    for (size_t i = 0; i + key_len + 3 < json_len; i++) {
        if (json[i] == '"' && memcmp(json + i + 1, key, key_len) == 0 &&
            json[i + 1 + key_len] == '"') {
            /* Skip ": or ":" */
            size_t j = i + 1 + key_len + 1;
            while (j < json_len && (json[j] == ':' || json[j] == ' '))
                j++;
            if (j < json_len && json[j] == '"') {
                j++;
                size_t start = j;
                while (j < json_len && json[j] != '"')
                    j++;
                size_t vlen = j - start;
                if (vlen >= cap)
                    vlen = cap - 1;
                memcpy(out, json + start, vlen);
                out[vlen] = '\0';
                return vlen;
            }
        }
    }
    return 0;
}
#endif /* HU_IS_TEST || HU_HTTP_CURL */

#if HU_IS_TEST
size_t hu_imessage_test_gif_json_extract(const char *json, size_t json_len, const char *key,
                                         char *out, size_t cap) {
    return gif_json_extract(json, json_len, key, out, cap);
}
#endif

#if !HU_IS_TEST && defined(HU_HTTP_CURL)
#include "human/core/http.h"
#include "human/core/json.h"

char *hu_imessage_fetch_gif(hu_allocator_t *alloc, const char *query, size_t query_len,
                            const char *api_key, size_t api_key_len) {
    if (!alloc || !query || query_len == 0 || !api_key || api_key_len == 0)
        return NULL;

    /* URL-encode the query: spaces to +, unreserved chars verbatim, rest %XX */
    char encoded[512];
    size_t eidx = 0;
    for (size_t i = 0; i < query_len && eidx + 3 < sizeof(encoded); i++) {
        unsigned char ch = (unsigned char)query[i];
        if (ch == ' ') {
            encoded[eidx++] = '+';
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded[eidx++] = (char)ch;
        } else {
            static const char hex[] = "0123456789ABCDEF";
            encoded[eidx++] = '%';
            encoded[eidx++] = hex[ch >> 4];
            encoded[eidx++] = hex[ch & 0x0F];
        }
    }
    encoded[eidx] = '\0';

    char url[512];
    int n = snprintf(url, sizeof(url),
                     "https://tenor.googleapis.com/v2/search?q=%s&key=%.*s"
                     "&client_key=human_app&limit=1&media_filter=gif",
                     encoded, (int)api_key_len, api_key);
    if (n < 0 || (size_t)n >= sizeof(url))
        return NULL;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, NULL, &resp);
    if (err != HU_OK || resp.status_code != 200 || !resp.body || resp.body_len == 0) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return NULL;
    }

    /* Extract the GIF URL from the JSON response.
     * Tenor v2 nests it at results[0].media_formats.gif.url */
    char gif_url[512];
    size_t gif_url_len = 0;

    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, resp.body, resp.body_len, &root) == HU_OK && root) {
        hu_json_value_t *results = hu_json_object_get(root, "results");
        hu_json_value_t *first = NULL;
        if (results && results->type == HU_JSON_ARRAY && results->data.array.len > 0)
            first = results->data.array.items[0];
        if (first && first->type == HU_JSON_OBJECT) {
            hu_json_value_t *media_formats = hu_json_object_get(first, "media_formats");
            if (media_formats && media_formats->type == HU_JSON_OBJECT) {
                hu_json_value_t *gif_obj = hu_json_object_get(media_formats, "gif");
                if (gif_obj && gif_obj->type == HU_JSON_OBJECT) {
                    const char *media_url = hu_json_get_string(gif_obj, "url");
                    if (media_url) {
                        size_t ulen = strlen(media_url);
                        if (ulen >= sizeof(gif_url))
                            ulen = sizeof(gif_url) - 1;
                        memcpy(gif_url, media_url, ulen);
                        gif_url[ulen] = '\0';
                        gif_url_len = ulen;
                    }
                }
            }
        }
        hu_json_free(alloc, root);
    }

    if (gif_url_len == 0) {
        /* Fallback if parse failed or response shape changed (field order, etc.) */
        const char *gif_section = NULL;
        for (size_t i = 0; i + 5 < resp.body_len; i++) {
            if (resp.body[i] == '"' && memcmp(resp.body + i, "\"gif\"", 5) == 0) {
                gif_section = resp.body + i;
                break;
            }
        }
        if (gif_section) {
            size_t remaining = resp.body_len - (size_t)(gif_section - resp.body);
            gif_url_len = gif_json_extract(gif_section, remaining, "url", gif_url, sizeof(gif_url));
        }
    }

    hu_http_response_free(alloc, &resp);

    if (gif_url_len == 0)
        return NULL;

    /* Download the GIF to a temp file */
    hu_http_response_t gif_resp = {0};
    err = hu_http_get(alloc, gif_url, NULL, &gif_resp);
    if (err != HU_OK || gif_resp.status_code != 200 || !gif_resp.body || gif_resp.body_len == 0) {
        if (gif_resp.owned && gif_resp.body)
            hu_http_response_free(alloc, &gif_resp);
        return NULL;
    }

    char tmp_path[256];
    static _Atomic unsigned gif_counter;
    unsigned gc = atomic_fetch_add(&gif_counter, 1);
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/human_gif_%u_%d_%u.gif", (unsigned)time(NULL),
             (int)getpid(), gc);

    /* /tmp GIF download buffer — short-lived, but still goes through
     * hu_io_secure_open so the path is checked for traversal and the
     * mode is explicit (0644 — GIF is shown to the user via the
     * channel). */
    FILE *f = NULL;
    if (hu_io_secure_open(tmp_path, HU_IO_PERM_USER, "wb", &f) != HU_OK || !f) {
        hu_http_response_free(alloc, &gif_resp);
        return NULL;
    }
    fwrite(gif_resp.body, 1, gif_resp.body_len, f);
    fclose(f);
    hu_http_response_free(alloc, &gif_resp);

    size_t path_len = strlen(tmp_path);
    char *result = (char *)alloc->alloc(alloc->ctx, path_len + 1);
    if (!result)
        return NULL;
    memcpy(result, tmp_path, path_len + 1);
    return result;
}
#else
char *hu_imessage_fetch_gif(hu_allocator_t *alloc, const char *query, size_t query_len,
                            const char *api_key, size_t api_key_len) {
    (void)alloc;
    (void)query;
    (void)query_len;
    (void)api_key;
    (void)api_key_len;
    return NULL;
}
#endif
