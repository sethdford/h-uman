#include "human/context/contact_style_overlay.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

size_t hu_temporal_mood_build(uint8_t hour, char *buf, size_t cap) {
    if (!buf || cap < 64)
        return 0;

    const char *mood;
    if (hour < 6)
        mood = "It's very late/early — you shouldn't be up. Brief and sleepy.";
    else if (hour < 9)
        mood = "It's early morning — you're probably just waking up. Terse and groggy.";
    else if (hour < 17)
        mood = "It's during work hours — you might be busy. Keep it professional-ish.";
    else if (hour < 21)
        mood = "It's evening — you're relaxed, more chatty.";
    else
        mood = "It's late night — you're winding down. Reflective.";

    int n = snprintf(buf, cap, "\n[Temporal context] %s\n", mood);
    if (n <= 0 || (size_t)n >= cap)
        return 0;
    return (size_t)n;
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include <sqlite3.h>

hu_error_t hu_contact_style_overlay_build(hu_allocator_t *alloc, hu_memory_t *memory,
                                          const char *contact_id, size_t contact_id_len,
                                          char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!memory || !contact_id || contact_id_len == 0)
        return HU_OK;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_OK;

    char display_name[128] = {0};
    char role[64] = {0};
    char laugh_style[32] = {0};
    int avg_msg_len = 0;
    bool uses_lowercase = false;
    bool uses_periods = false;
    bool has_style = false;

    /* Query contact_identities for display_name */
    {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(
            db,
            "SELECT display_name FROM contact_identities WHERE contact_id=? LIMIT 1",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *dn = (const char *)sqlite3_column_text(stmt, 0);
                if (dn && dn[0])
                    snprintf(display_name, sizeof(display_name), "%s", dn);
            }
            sqlite3_finalize(stmt);
        }
    }

    /* Query contact_relationships for role */
    {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(
            db,
            "SELECT role FROM contact_relationships WHERE contact_id=? "
            "ORDER BY last_mentioned DESC LIMIT 1",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *r = (const char *)sqlite3_column_text(stmt, 0);
                if (r && r[0])
                    snprintf(role, sizeof(role), "%s", r);
            }
            sqlite3_finalize(stmt);
        }
    }

    /* Query style_fingerprints */
    {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(
            db,
            "SELECT uses_lowercase, uses_periods, laugh_style, avg_message_length "
            "FROM style_fingerprints WHERE contact_id=?",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                uses_lowercase = sqlite3_column_int(stmt, 0) != 0;
                uses_periods = sqlite3_column_int(stmt, 1) != 0;
                const char *ls = (const char *)sqlite3_column_text(stmt, 2);
                if (ls && ls[0])
                    snprintf(laugh_style, sizeof(laugh_style), "%s", ls);
                avg_msg_len = sqlite3_column_int(stmt, 3);
                has_style = true;
            }
            sqlite3_finalize(stmt);
        }
    }

    if (!display_name[0] && !role[0] && !has_style)
        return HU_OK;

    /* Build the overlay string */
#define OVERLAY_CAP 1024
    char *buf = (char *)alloc->alloc(alloc->ctx, OVERLAY_CAP);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    pos = hu_buf_appendf(buf, OVERLAY_CAP, pos, "\n[Contact style overlay] ");

    if (display_name[0]) {
        pos = hu_buf_appendf(buf, OVERLAY_CAP, pos, "You're texting %s", display_name);
        if (role[0])
            pos = hu_buf_appendf(buf, OVERLAY_CAP, pos, ", %s", role);
        pos = hu_buf_appendf(buf, OVERLAY_CAP, pos, ". ");
    }

    if (has_style) {
        if (avg_msg_len > 0)
            pos = hu_buf_appendf(buf, OVERLAY_CAP, pos,
                                 "Their avg message length is ~%d chars — match the vibe. ",
                                 avg_msg_len);
        if (laugh_style[0])
            pos = hu_buf_appendf(buf, OVERLAY_CAP, pos,
                                 "Their laugh style is '%s'. ", laugh_style);
        if (uses_lowercase && !uses_periods)
            pos = hu_buf_appendf(buf, OVERLAY_CAP, pos, "They text lowercase, no periods. ");
        else if (uses_lowercase)
            pos = hu_buf_appendf(buf, OVERLAY_CAP, pos, "They text in lowercase. ");
        else if (!uses_periods)
            pos = hu_buf_appendf(buf, OVERLAY_CAP, pos, "They tend to skip periods. ");
    }

    pos = hu_buf_appendf(buf, OVERLAY_CAP, pos, "\n");

    if (pos == 0 || pos >= OVERLAY_CAP) {
        alloc->free(alloc->ctx, buf, OVERLAY_CAP);
        return HU_OK;
    }

    /* Shrink to fit */
    size_t need = pos + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, OVERLAY_CAP, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, OVERLAY_CAP);
        return HU_ERR_OUT_OF_MEMORY;
    }
    shrunk[pos] = '\0';
    *out = shrunk;
    *out_len = pos;
#undef OVERLAY_CAP
    return HU_OK;
}

hu_error_t hu_contact_emotional_context_build(hu_allocator_t *alloc, hu_memory_t *memory,
                                              const char *contact_id, size_t contact_id_len,
                                              size_t max_entries, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!memory || !contact_id || contact_id_len == 0)
        return HU_OK;
    if (max_entries == 0)
        max_entries = 5;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_OK;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT topic, emotion, intensity FROM emotional_moments "
        "WHERE contact_id=? ORDER BY created_at DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_OK;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)max_entries);

#define EMO_CAP 1024
    char buf[EMO_CAP];
    size_t pos = 0;
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_entries) {
        const char *topic = (const char *)sqlite3_column_text(stmt, 0);
        const char *emotion = (const char *)sqlite3_column_text(stmt, 1);
        double intensity = sqlite3_column_double(stmt, 2);

        if (!topic || !emotion)
            continue;

        if (count == 0)
            pos = hu_buf_appendf(buf, EMO_CAP, pos,
                                 "\n[Recent emotional context with this person]\n");

        pos = hu_buf_appendf(buf, EMO_CAP, pos, "- %s: %s (intensity %.1f)\n",
                             topic, emotion, intensity);
        count++;
    }
    sqlite3_finalize(stmt);
#undef EMO_CAP

    if (count == 0)
        return HU_OK;

    size_t need = pos + 1;
    char *result = (char *)alloc->alloc(alloc->ctx, need);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(result, buf, pos);
    result[pos] = '\0';
    *out = result;
    *out_len = pos;
    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_contact_style_overlay_build(hu_allocator_t *alloc, hu_memory_t *memory,
                                          const char *contact_id, size_t contact_id_len,
                                          char **out, size_t *out_len) {
    (void)alloc;
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_OK;
}

hu_error_t hu_contact_emotional_context_build(hu_allocator_t *alloc, hu_memory_t *memory,
                                              const char *contact_id, size_t contact_id_len,
                                              size_t max_entries, char **out, size_t *out_len) {
    (void)alloc;
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    (void)max_entries;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
