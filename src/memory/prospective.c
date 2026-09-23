typedef int hu_prospective_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/memory/prospective.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* Same intention already collected: identical action for the same contact
 * (NULL contact and "" are both "any contact" here). */
static bool prospective_seen(const hu_prospective_entry_t *arr, size_t n,
                             const hu_prospective_entry_t *e) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(arr[i].action, e->action) == 0 && strcmp(arr[i].contact_id, e->contact_id) == 0)
            return true;
    }
    return false;
}

hu_error_t hu_prospective_check_triggers(hu_allocator_t *alloc, sqlite3 *db,
                                         const char *trigger_type, const char *trigger_value,
                                         size_t tv_len, const char *contact_id, size_t cid_len,
                                         int64_t now_ts, hu_prospective_entry_t **out,
                                         size_t *out_count) {
    if (!alloc || !db || !trigger_type || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sqlite3_stmt *stmt = NULL;
    /* The keyword test is done in C with hu_str_contains_word_ci_n, not with
     * instr(): a cue is a whole word or phrase. The substring form fired
     * "work" on "bath and body works" (live, 2026-09-12); the earlier
     * case-sensitive form never fired at all. Newest intention first so the
     * caller's render cap (3) does not pin the oldest rows forever. */
    int rc = sqlite3_prepare_v2(db,
                                "SELECT id,trigger_type,trigger_value,action,contact_id,expires_at,"
                                "created_at FROM prospective_memories WHERE fired=0 AND "
                                "trigger_type=? AND (contact_id IS NULL OR contact_id=?) AND "
                                "(expires_at IS NULL OR expires_at=0 OR expires_at>?) "
                                "ORDER BY created_at DESC, id DESC",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, trigger_type, -1, SQLITE_STATIC);
    if (contact_id && cid_len > 0)
        sqlite3_bind_text(stmt, 2, contact_id, (int)cid_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int64(stmt, 3, now_ts);

    size_t cap = 16;
    hu_prospective_entry_t *arr =
        (hu_prospective_entry_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_prospective_entry_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, cap * sizeof(hu_prospective_entry_t));
    size_t n = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            size_t new_cap = cap * 2;
            hu_prospective_entry_t *nb = (hu_prospective_entry_t *)alloc->realloc(
                alloc->ctx, arr, cap * sizeof(hu_prospective_entry_t),
                new_cap * sizeof(hu_prospective_entry_t));
            if (!nb) {
                for (size_t i = 0; i < n; i++)
                    (void)0;
                alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_entry_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            arr = nb;
            cap = new_cap;
        }
        hu_prospective_entry_t *e = &arr[n];
        e->id = sqlite3_column_int64(stmt, 0);
        const char *tt = (const char *)sqlite3_column_text(stmt, 1);
        if (tt) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 1);
            memcpy(e->trigger_type, tt, MIN(len, sizeof(e->trigger_type) - 1));
            e->trigger_type[MIN(len, sizeof(e->trigger_type) - 1)] = '\0';
        }
        const char *tv = (const char *)sqlite3_column_text(stmt, 2);
        if (tv) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 2);
            memcpy(e->trigger_value, tv, MIN(len, sizeof(e->trigger_value) - 1));
            e->trigger_value[MIN(len, sizeof(e->trigger_value) - 1)] = '\0';
        }
        const char *act = (const char *)sqlite3_column_text(stmt, 3);
        if (act) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 3);
            memcpy(e->action, act, MIN(len, sizeof(e->action) - 1));
            e->action[MIN(len, sizeof(e->action) - 1)] = '\0';
        }
        const char *cid = (const char *)sqlite3_column_text(stmt, 4);
        if (cid) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 4);
            memcpy(e->contact_id, cid, MIN(len, sizeof(e->contact_id) - 1));
            e->contact_id[MIN(len, sizeof(e->contact_id) - 1)] = '\0';
        }
        e->expires_at = sqlite3_column_int64(stmt, 5);
        e->created_at = sqlite3_column_int64(stmt, 6);
        if (e->trigger_value[0] == '\0' ||
            !hu_str_contains_word_ci_n(trigger_value, tv_len, e->trigger_value) ||
            prospective_seen(arr, n, e)) {
            memset(e, 0, sizeof(*e));
            continue;
        }
        n++;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_entry_t));
        return HU_ERR_MEMORY_BACKEND;
    }

    if (n == 0) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_entry_t));
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }
    *out = arr;
    *out_count = n;
    return HU_OK;
}

hu_error_t hu_prospective_mark_fired(sqlite3 *db, const hu_prospective_entry_t *entries,
                                     size_t count) {
    if (!db || (count > 0 && !entries))
        return HU_ERR_INVALID_ARGUMENT;
    if (count == 0)
        return HU_OK;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "UPDATE prospective_memories SET fired=1 WHERE fired=0 AND "
                                "action=?1 AND ((?2 IS NULL AND contact_id IS NULL) OR "
                                "contact_id=?2)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    hu_error_t err = HU_OK;
    for (size_t i = 0; i < count; i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, entries[i].action, -1, SQLITE_STATIC);
        if (entries[i].contact_id[0] != '\0')
            sqlite3_bind_text(stmt, 2, entries[i].contact_id, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(stmt, 2);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            err = HU_ERR_MEMORY_BACKEND;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return err;
}
#define PROSPECTIVE_RENDER_CAP 3

char *hu_prospective_directive_build(hu_allocator_t *alloc, sqlite3 *db, const char *text,
                                     size_t text_len, const char *contact_id, size_t cid_len,
                                     int64_t now_ts, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc || !db || !text || text_len == 0 || !out_len)
        return NULL;

    hu_prospective_entry_t *entries = NULL;
    size_t count = 0;
    if (hu_prospective_check_triggers(alloc, db, "keyword", text, text_len, contact_id, cid_len,
                                      now_ts, &entries, &count) != HU_OK ||
        !entries || count == 0)
        return NULL;

    char buf[1024];
    size_t pos = 0;
    int n = snprintf(buf, sizeof(buf), "[PROSPECTIVE MEMORY: Remember to: ");
    if (n > 0 && (size_t)n < sizeof(buf))
        pos = (size_t)n;
    size_t rendered = 0;
    for (size_t i = 0; i < count && i < PROSPECTIVE_RENDER_CAP && pos < sizeof(buf) - 64; i++) {
        if (i > 0) {
            memcpy(buf + pos, " | ", 3);
            pos += 3;
        }
        int w = snprintf(buf + pos, sizeof(buf) - pos, "%s (triggered by: %s)", entries[i].action,
                         entries[i].trigger_value);
        if (w <= 0 || pos + (size_t)w >= sizeof(buf))
            break;
        pos += (size_t)w;
        rendered++;
    }
    char *out = NULL;
    if (rendered > 0 && pos + 2 < sizeof(buf)) {
        buf[pos++] = ']';
        buf[pos] = '\0';
        out = (char *)alloc->alloc(alloc->ctx, pos + 1);
        if (out) {
            memcpy(out, buf, pos + 1);
            *out_len = pos;
            /* A reminder is surfaced once: retire every keyword of the rendered
             * intentions so the next text does not re-inject them. */
            if (hu_prospective_mark_fired(db, entries, rendered) != HU_OK)
                hu_log_warn("prospective", NULL, "could not retire %zu surfaced triggers",
                            rendered);
            hu_log_info("prospective", NULL,
                        "fired %zu of %zu open triggers for %.*s: %s (cue: %s)", rendered, count,
                        (int)cid_len, contact_id ? contact_id : "", entries[0].action,
                        entries[0].trigger_value);
        }
    }
    alloc->free(alloc->ctx, entries, count * sizeof(hu_prospective_entry_t));
    return out;
}

hu_error_t hu_prospective_expire_sweep(sqlite3 *db, int64_t now_ts, int64_t *out_n) {
    if (out_n)
        *out_n = 0;
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "UPDATE prospective_memories SET fired=3 WHERE fired=0 AND "
                                "expires_at > 0 AND expires_at <= ?1",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_int64(stmt, 1, now_ts);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    if (out_n)
        *out_n = (int64_t)sqlite3_changes(db);
    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

#include "human/core/error.h"
#include "human/memory/prospective.h"
hu_error_t hu_prospective_check_triggers(hu_allocator_t *alloc, void *db, const char *trigger_type,
                                         const char *trigger_value, size_t tv_len,
                                         const char *contact_id, size_t cid_len, int64_t now_ts,
                                         hu_prospective_entry_t **out, size_t *out_count) {
    (void)alloc;
    (void)db;
    (void)trigger_type;
    (void)trigger_value;
    (void)tv_len;
    (void)contact_id;
    (void)cid_len;
    (void)now_ts;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_prospective_mark_fired(void *db, const void *entries, size_t count) {
    (void)db;
    (void)entries;
    (void)count;
    return HU_ERR_NOT_SUPPORTED;
}
#endif /* HU_ENABLE_SQLITE */
