#include "human/persona/auto_profile.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/persona.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

#if !defined(_WIN32)
#include <unistd.h>
#endif

#define HU_AUTO_PROFILE_MSG_LIMIT 500

char *hu_persona_profile_describe_style(hu_allocator_t *alloc,
                                        const hu_sampler_contact_stats_t *stats,
                                        const char *contact_id, size_t contact_id_len,
                                        size_t *out_len) {
    if (!alloc || !stats || !out_len)
        return NULL;
    *out_len = 0;

    char buf[512];
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "Based on their texting: they send %s messages (avg %zu chars)",
                            stats->avg_their_len < 30   ? "short"
                            : stats->avg_their_len < 80 ? "medium-length"
                                                        : "long",
                            stats->avg_their_len);
    if (stats->uses_emoji)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", use emoji frequently");
    else
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", rarely use emoji");
    if (stats->texts_in_bursts)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", text in bursts of 2-3 messages");
    if (stats->prefers_short)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ". Mirror their energy and brevity.");
    else
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ".");

    (void)contact_id;
    (void)contact_id_len;

    char *out = hu_strndup(alloc, buf, pos);
    if (out)
        *out_len = pos;
    return out;
}

hu_error_t hu_persona_auto_profile(hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, hu_persona_overlay_t *overlay) {
    if (!alloc || !contact_id || !overlay)
        return HU_ERR_INVALID_ARGUMENT;
    memset(overlay, 0, sizeof(*overlay));

#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Mock overlay for tests */
    overlay->formality = hu_strndup(alloc, "casual", 6);
    overlay->avg_length = hu_strndup(alloc, "35", 2);
    overlay->emoji_usage = hu_strndup(alloc, "moderate", 8);
    overlay->message_splitting = false;
    overlay->max_segment_chars = 160;
    return HU_OK;
#endif

#if !defined(__APPLE__) || !defined(__MACH__)
    return HU_ERR_NOT_SUPPORTED;
#endif

#if defined(HU_ENABLE_SQLITE)
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return HU_ERR_NOT_FOUND;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return HU_ERR_INVALID_ARGUMENT;

    if (access(db_path, R_OK) != 0)
        return HU_ERR_NOT_FOUND;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_NOT_FOUND;
    }

    const char *sql = "SELECT m.text, m.is_from_me, m.date "
                      "FROM message m "
                      "LEFT JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.text IS NOT NULL AND m.text != '' "
                      "ORDER BY m.date ASC LIMIT ?2";

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
    sqlite3_bind_text(stmt, 1, contact_buf, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)HU_AUTO_PROFILE_MSG_LIMIT);

    size_t raw_cap = HU_AUTO_PROFILE_MSG_LIMIT;
    hu_sampler_raw_msg_t *raw =
        (hu_sampler_raw_msg_t *)alloc->alloc(alloc->ctx, raw_cap * sizeof(*raw));
    char **text_bufs = (char **)alloc->alloc(alloc->ctx, raw_cap * sizeof(char *));
    if (!raw || !text_bufs) {
        if (raw)
            alloc->free(alloc->ctx, raw, raw_cap * sizeof(*raw));
        if (text_bufs)
            alloc->free(alloc->ctx, text_bufs, raw_cap * sizeof(char *));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(raw, 0, raw_cap * sizeof(*raw));
    memset(text_bufs, 0, raw_cap * sizeof(char *));
    size_t raw_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && raw_count < raw_cap) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        int from_me = sqlite3_column_int(stmt, 1);
        int64_t date = sqlite3_column_int64(stmt, 2);
        if (text && text[0]) {
            text_bufs[raw_count] = hu_strdup(alloc, text);
            if (!text_bufs[raw_count])
                break;
            raw[raw_count].text = text_bufs[raw_count];
            raw[raw_count].text_len = strlen(text_bufs[raw_count]);
            raw[raw_count].is_from_me = (from_me != 0);
            raw[raw_count].timestamp = date / 1000000000;
            raw_count++;
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    hu_sampler_contact_stats_t stats;
    hu_error_t err = hu_persona_sampler_detect_contact(alloc, raw, raw_count, &stats);

    for (size_t i = 0; i < raw_count; i++) {
        if (text_bufs[i])
            alloc->free(alloc->ctx, text_bufs[i], strlen(text_bufs[i]) + 1);
    }
    alloc->free(alloc->ctx, text_bufs, raw_cap * sizeof(char *));
    alloc->free(alloc->ctx, raw, raw_cap * sizeof(*raw));

    if (err != HU_OK)
        return err;

    /* Map stats to overlay */
    if (stats.avg_their_len < 30) {
        overlay->formality = hu_strndup(alloc, "casual", 6);
        overlay->avg_length = hu_strndup(alloc, "20", 2);
        overlay->message_splitting = true;
        overlay->max_segment_chars = 80;
    } else if (stats.avg_their_len <= 80) {
        overlay->formality = hu_strndup(alloc, "neutral", 7);
        overlay->avg_length = hu_strndup(alloc, "50", 2);
        overlay->message_splitting = false;
        overlay->max_segment_chars = 160;
    } else {
        overlay->formality = hu_strndup(alloc, "formal", 6);
        overlay->avg_length = hu_strndup(alloc, "80", 2);
        overlay->message_splitting = false;
        overlay->max_segment_chars = 320;
    }

    overlay->emoji_usage =
        stats.uses_emoji ? hu_strndup(alloc, "frequent", 8) : hu_strndup(alloc, "rare", 4);

    if (stats.texts_in_bursts)
        overlay->message_splitting = true;

    if (stats.prefers_short) {
        overlay->typing_quirks = (char **)alloc->alloc(alloc->ctx, 2 * sizeof(char *));
        if (overlay->typing_quirks) {
            overlay->typing_quirks[0] = hu_strndup(alloc, "lowercase", 9);
            overlay->typing_quirks[1] = hu_strndup(alloc, "no_periods", 10);
            overlay->typing_quirks_count = 2;
        }
    }

    size_t desc_len = 0;
    char *desc =
        hu_persona_profile_describe_style(alloc, &stats, contact_id, contact_id_len, &desc_len);
    if (desc && desc_len > 0) {
        overlay->style_notes = (char **)alloc->alloc(alloc->ctx, sizeof(char *));
        if (overlay->style_notes) {
            overlay->style_notes[0] = desc;
            overlay->style_notes_count = 1;
        } else {
            alloc->free(alloc->ctx, desc, desc_len + 1);
        }
    }

    return HU_OK;
#else
    (void)contact_id_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}
