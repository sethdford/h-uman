#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/superhuman.h"
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_SUPERHUMAN_COPY_STR(dst, dst_size, src, src_len) \
    do { \
        size_t _copy = (src_len) < (dst_size) - 1 ? (src_len) : (dst_size) - 1; \
        if ((src) && _copy > 0) { \
            memcpy((dst), (src), _copy); \
            (dst)[_copy] = '\0'; \
        } else { \
            (dst)[0] = '\0'; \
        } \
    } while (0)

static sqlite3 *get_db(void *sqlite_ctx) {
    hu_memory_t *mem = (hu_memory_t *)sqlite_ctx;
    if (!mem)
        return NULL;
    return hu_sqlite_memory_get_db(mem);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Inside jokes
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_inside_joke_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *context, size_t context_len,
    const char *punchline, size_t punchline_len) {
    (void)alloc;
    if (!sqlite_ctx || !contact_id || contact_id_len == 0 || !context || context_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO inside_jokes(contact_id,context,punchline,created_at,last_referenced,"
        "reference_count) VALUES(?,?,?,?,?,0)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_text(stmt, 2, context, (int)context_len, NULL);
    sqlite3_bind_text(stmt, 3, punchline ? punchline : "", punchline ? (int)punchline_len : 0, NULL);
    sqlite3_bind_int64(stmt, 4, now_ts);
    sqlite3_bind_int64(stmt, 5, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_superhuman_inside_joke_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit,
    hu_inside_joke_t **out, size_t *out_count) {
    if (!sqlite_ctx || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id,contact_id,context,punchline,created_at,last_referenced,reference_count "
        "FROM inside_jokes WHERE contact_id=? ORDER BY last_referenced DESC, created_at DESC "
        "LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_int64(stmt, 2, (int64_t)(limit > 0 ? limit : 100));

    size_t cap = 16;
    size_t count = 0;
    hu_inside_joke_t *arr =
        (hu_inside_joke_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_inside_joke_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            hu_inside_joke_t *n =
                (hu_inside_joke_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_inside_joke_t));
            if (!n) {
                alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_inside_joke_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(n, arr, count * sizeof(hu_inside_joke_t));
            alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_inside_joke_t));
            arr = n;
        }
        hu_inside_joke_t *e = &arr[count];
        memset(e, 0, sizeof(*e));
        e->id = sqlite3_column_int64(stmt, 0);
        const char *cid = (const char *)sqlite3_column_text(stmt, 1);
        size_t cid_len = cid ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
        HU_SUPERHUMAN_COPY_STR(e->contact_id, sizeof(e->contact_id), cid, cid_len);
        const char *ctx = (const char *)sqlite3_column_text(stmt, 2);
        size_t ctx_len = ctx ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
        HU_SUPERHUMAN_COPY_STR(e->context, sizeof(e->context), ctx, ctx_len);
        const char *pl = (const char *)sqlite3_column_text(stmt, 3);
        size_t pl_len = pl ? (size_t)sqlite3_column_bytes(stmt, 3) : 0;
        HU_SUPERHUMAN_COPY_STR(e->punchline, sizeof(e->punchline), pl, pl_len);
        e->created_at = sqlite3_column_int64(stmt, 4);
        e->last_referenced = sqlite3_column_int64(stmt, 5);
        e->reference_count = (uint32_t)sqlite3_column_int(stmt, 6);
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0 && arr) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_inside_joke_t));
        arr = NULL;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_superhuman_inside_joke_reference(void *sqlite_ctx, int64_t id) {
    if (!sqlite_ctx)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE inside_jokes SET last_referenced=?, reference_count=reference_count+1 WHERE id=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, now_ts);
    sqlite3_bind_int64(stmt, 2, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

void hu_superhuman_inside_joke_free(hu_allocator_t *alloc, hu_inside_joke_t *arr, size_t count) {
    if (alloc && arr)
        alloc->free(alloc->ctx, arr, count * sizeof(hu_inside_joke_t));
}

/* ──────────────────────────────────────────────────────────────────────────
 * Commitments
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_commitment_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *description, size_t desc_len,
    const char *who, size_t who_len, int64_t deadline) {
    (void)alloc;
    if (!sqlite_ctx || !contact_id || contact_id_len == 0 || !description || desc_len == 0 ||
        !who || who_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO commitments(contact_id,description,who,deadline,status,created_at,"
        "followed_up_at) VALUES(?,?,?,?,'pending',?,NULL)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_text(stmt, 2, description, (int)desc_len, NULL);
    sqlite3_bind_text(stmt, 3, who, (int)who_len, NULL);
    sqlite3_bind_int64(stmt, 4, deadline);
    sqlite3_bind_int64(stmt, 5, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_superhuman_commitment_list_due(void *sqlite_ctx, hu_allocator_t *alloc,
    int64_t now_ts, size_t limit, hu_superhuman_commitment_t **out, size_t *out_count) {
    if (!sqlite_ctx || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id,contact_id,description,who,deadline,status,created_at,followed_up_at "
        "FROM commitments WHERE status='pending' AND deadline IS NOT NULL AND deadline<=? "
        "ORDER BY deadline LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, now_ts);
    sqlite3_bind_int64(stmt, 2, (int64_t)(limit > 0 ? limit : 100));

    size_t cap = 16;
    size_t count = 0;
    hu_superhuman_commitment_t *arr =
        (hu_superhuman_commitment_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_superhuman_commitment_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            hu_superhuman_commitment_t *n =
                (hu_superhuman_commitment_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_superhuman_commitment_t));
            if (!n) {
                alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_superhuman_commitment_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(n, arr, count * sizeof(hu_superhuman_commitment_t));
            alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_superhuman_commitment_t));
            arr = n;
        }
        hu_superhuman_commitment_t *e = &arr[count];
        memset(e, 0, sizeof(*e));
        e->id = sqlite3_column_int64(stmt, 0);
        const char *cid = (const char *)sqlite3_column_text(stmt, 1);
        size_t cid_len = cid ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
        HU_SUPERHUMAN_COPY_STR(e->contact_id, sizeof(e->contact_id), cid, cid_len);
        const char *desc = (const char *)sqlite3_column_text(stmt, 2);
        size_t desc_len = desc ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
        HU_SUPERHUMAN_COPY_STR(e->description, sizeof(e->description), desc, desc_len);
        const char *w = (const char *)sqlite3_column_text(stmt, 3);
        size_t w_len = w ? (size_t)sqlite3_column_bytes(stmt, 3) : 0;
        HU_SUPERHUMAN_COPY_STR(e->who, sizeof(e->who), w, w_len);
        e->deadline = sqlite3_column_int64(stmt, 4);
        const char *st = (const char *)sqlite3_column_text(stmt, 5);
        size_t st_len = st ? (size_t)sqlite3_column_bytes(stmt, 5) : 0;
        HU_SUPERHUMAN_COPY_STR(e->status, sizeof(e->status), st, st_len);
        e->created_at = sqlite3_column_int64(stmt, 6);
        e->followed_up_at = sqlite3_column_int64(stmt, 7);
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0 && arr) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_superhuman_commitment_t));
        arr = NULL;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_superhuman_commitment_mark_followed_up(void *sqlite_ctx, int64_t id) {
    if (!sqlite_ctx)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE commitments SET status='followed_up', followed_up_at=? WHERE id=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, now_ts);
    sqlite3_bind_int64(stmt, 2, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

void hu_superhuman_commitment_free(hu_allocator_t *alloc, hu_superhuman_commitment_t *arr,
    size_t count) {
    if (alloc && arr)
        alloc->free(alloc->ctx, arr, count * sizeof(hu_superhuman_commitment_t));
}

/* ──────────────────────────────────────────────────────────────────────────
 * Temporal patterns
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_temporal_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, int day_of_week, int hour, int64_t response_time_ms) {
    if (!sqlite_ctx || !contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT message_count, avg_response_time_ms FROM temporal_patterns "
        "WHERE contact_id=? AND day_of_week=? AND hour=?",
        -1, &sel, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(sel, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_int(sel, 2, day_of_week);
    sqlite3_bind_int(sel, 3, hour);

    int old_count = 0;
    int64_t old_avg = 0;
    rc = sqlite3_step(sel);
    if (rc == SQLITE_ROW) {
        old_count = sqlite3_column_int(sel, 0);
        old_avg = sqlite3_column_int64(sel, 1);
    }
    sqlite3_finalize(sel);

    int new_count = old_count + 1;
    int64_t new_avg = (old_avg * (int64_t)old_count + response_time_ms) / (int64_t)new_count;

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO temporal_patterns(contact_id,day_of_week,hour,message_count,"
        "avg_response_time_ms) VALUES(?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_int(stmt, 2, day_of_week);
    sqlite3_bind_int(stmt, 3, hour);
    sqlite3_bind_int(stmt, 4, new_count);
    sqlite3_bind_int64(stmt, 5, new_avg);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_superhuman_temporal_get_quiet_hours(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, int *out_day, int *out_hour_start,
    int *out_hour_end) {
    (void)alloc;
    if (!sqlite_ctx || !contact_id || contact_id_len == 0 || !out_day || !out_hour_start ||
        !out_hour_end)
        return HU_ERR_INVALID_ARGUMENT;

    *out_day = 0;
    *out_hour_start = 0;
    *out_hour_end = 1;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT day_of_week, hour, message_count FROM temporal_patterns "
        "WHERE contact_id=? ORDER BY message_count ASC, avg_response_time_ms DESC LIMIT 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_day = sqlite3_column_int(stmt, 0);
        *out_hour_start = sqlite3_column_int(stmt, 1);
        *out_hour_end = *out_hour_start + 1;
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Delayed follow-ups
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_delayed_followup_schedule(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *topic, size_t topic_len,
    int64_t scheduled_at) {
    (void)alloc;
    if (!sqlite_ctx || !contact_id || contact_id_len == 0 || !topic || topic_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) VALUES(?,?,?,0)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_text(stmt, 2, topic, (int)topic_len, NULL);
    sqlite3_bind_int64(stmt, 3, scheduled_at);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_superhuman_delayed_followup_list_due(void *sqlite_ctx, hu_allocator_t *alloc,
    int64_t now_ts, hu_delayed_followup_t **out, size_t *out_count) {
    if (!sqlite_ctx || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id,contact_id,topic,scheduled_at,sent FROM delayed_followups "
        "WHERE scheduled_at<=? AND sent=0 ORDER BY scheduled_at",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, now_ts);

    size_t cap = 16;
    size_t count = 0;
    hu_delayed_followup_t *arr =
        (hu_delayed_followup_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_delayed_followup_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            hu_delayed_followup_t *n =
                (hu_delayed_followup_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_delayed_followup_t));
            if (!n) {
                alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_delayed_followup_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(n, arr, count * sizeof(hu_delayed_followup_t));
            alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_delayed_followup_t));
            arr = n;
        }
        hu_delayed_followup_t *e = &arr[count];
        memset(e, 0, sizeof(*e));
        e->id = sqlite3_column_int64(stmt, 0);
        const char *cid = (const char *)sqlite3_column_text(stmt, 1);
        size_t cid_len = cid ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
        HU_SUPERHUMAN_COPY_STR(e->contact_id, sizeof(e->contact_id), cid, cid_len);
        const char *top = (const char *)sqlite3_column_text(stmt, 2);
        size_t top_len = top ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
        HU_SUPERHUMAN_COPY_STR(e->topic, sizeof(e->topic), top, top_len);
        e->scheduled_at = sqlite3_column_int64(stmt, 3);
        e->sent = sqlite3_column_int(stmt, 4) != 0;
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0 && arr) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_delayed_followup_t));
        arr = NULL;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_superhuman_delayed_followup_mark_sent(void *sqlite_ctx, int64_t id) {
    if (!sqlite_ctx)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE delayed_followups SET sent=1 WHERE id=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

void hu_superhuman_delayed_followup_free(hu_allocator_t *alloc, hu_delayed_followup_t *arr,
    size_t count) {
    if (alloc && arr)
        alloc->free(alloc->ctx, arr, count * sizeof(hu_delayed_followup_t));
}

/* ──────────────────────────────────────────────────────────────────────────
 * Micro-moments
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_micro_moment_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *fact, size_t fact_len,
    const char *significance, size_t sig_len) {
    (void)alloc;
    if (!sqlite_ctx || !contact_id || contact_id_len == 0 || !fact || fact_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO micro_moments(contact_id,fact,significance,created_at) VALUES(?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_text(stmt, 2, fact, (int)fact_len, NULL);
    sqlite3_bind_text(stmt, 3, significance ? significance : "", significance ? (int)sig_len : 0, NULL);
    sqlite3_bind_int64(stmt, 4, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static hu_error_t append_formatted(char **buf, size_t *len, size_t *cap, hu_allocator_t *alloc,
    const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;
    size_t need = *len + (size_t)n + 1;
    if (need > *cap) {
        size_t new_cap = *cap ? *cap * 2 : 256;
        if (new_cap < need)
            new_cap = need;
        char *new_buf = (char *)alloc->alloc(alloc->ctx, new_cap);
        if (!new_buf)
            return HU_ERR_OUT_OF_MEMORY;
        if (*buf) {
            memcpy(new_buf, *buf, *len + 1);
            alloc->free(alloc->ctx, *buf, *cap);
        } else {
            new_buf[0] = '\0';
        }
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, tmp, (size_t)n + 1);
    *len += (size_t)n;
    return HU_OK;
}

hu_error_t hu_superhuman_micro_moment_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len) {
    if (!sqlite_ctx || !alloc || !contact_id || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_json = NULL;
    *out_len = 0;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT fact, significance FROM micro_moments WHERE contact_id=? "
        "ORDER BY created_at DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_int64(stmt, 2, (int64_t)(limit > 0 ? limit : 100));

    size_t buf_len = 0;
    size_t buf_cap = 0;
    char *buf = NULL;
    hu_error_t err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "Micro-moments:\n");
    if (err != HU_OK) {
        sqlite3_finalize(stmt);
        return err;
    }

    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *fact = (const char *)sqlite3_column_text(stmt, 0);
        const char *sig = (const char *)sqlite3_column_text(stmt, 1);
        const char *fact_s = fact ? fact : "";
        const char *sig_s = sig && sig[0] ? sig : "(no significance)";
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc,
            "- %s | %s\n", fact_s, sig_s);
        if (err != HU_OK) {
            if (buf)
                alloc->free(alloc->ctx, buf, buf_cap);
            sqlite3_finalize(stmt);
            return err;
        }
        row_count++;
    }
    sqlite3_finalize(stmt);

    if (row_count == 0 && buf) {
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "(none)\n");
        if (err != HU_OK) {
            alloc->free(alloc->ctx, buf, buf_cap);
            return err;
        }
    }
    *out_json = buf;
    *out_len = buf_cap; /* allocated size for caller to free */
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Avoidance patterns
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_avoidance_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len, bool topic_changed_quickly) {
    if (!sqlite_ctx || !contact_id || contact_id_len == 0 || !topic || topic_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT mention_count, change_count FROM avoidance_patterns "
        "WHERE contact_id=? AND topic=?",
        -1, &sel, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(sel, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_text(sel, 2, topic, (int)topic_len, NULL);

    int mention = 1;
    int change = topic_changed_quickly ? 1 : 0;
    rc = sqlite3_step(sel);
    if (rc == SQLITE_ROW) {
        mention = sqlite3_column_int(sel, 0) + 1;
        change = sqlite3_column_int(sel, 1) + (topic_changed_quickly ? 1 : 0);
    }
    sqlite3_finalize(sel);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO avoidance_patterns(contact_id,topic,mention_count,change_count,"
        "last_mentioned) VALUES(?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_text(stmt, 2, topic, (int)topic_len, NULL);
    sqlite3_bind_int(stmt, 3, mention);
    sqlite3_bind_int(stmt, 4, change);
    sqlite3_bind_int64(stmt, 5, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_superhuman_avoidance_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, char **out_json, size_t *out_len) {
    if (!sqlite_ctx || !alloc || !contact_id || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_json = NULL;
    *out_len = 0;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT topic, mention_count, change_count FROM avoidance_patterns WHERE contact_id=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);

    size_t buf_len = 0;
    size_t buf_cap = 0;
    char *buf = NULL;
    hu_error_t err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "Avoidance patterns:\n");
    if (err != HU_OK) {
        sqlite3_finalize(stmt);
        return err;
    }

    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *topic = (const char *)sqlite3_column_text(stmt, 0);
        int mention = sqlite3_column_int(stmt, 1);
        int change = sqlite3_column_int(stmt, 2);
        const char *t = topic ? topic : "";
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc,
            "- %s (mentions: %d, changes: %d)\n", t, mention, change);
        if (err != HU_OK) {
            if (buf)
                alloc->free(alloc->ctx, buf, buf_cap);
            sqlite3_finalize(stmt);
            return err;
        }
        row_count++;
    }
    sqlite3_finalize(stmt);

    if (row_count == 0 && buf) {
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "(none)\n");
        if (err != HU_OK) {
            alloc->free(alloc->ctx, buf, buf_cap);
            return err;
        }
    }
    *out_json = buf;
    *out_len = buf_cap; /* allocated size for caller to free */
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Topic baselines
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_topic_baseline_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len) {
    if (!sqlite_ctx || !contact_id || contact_id_len == 0 || !topic || topic_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT mention_count FROM topic_baselines WHERE contact_id=? AND topic=?",
        -1, &sel, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(sel, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_text(sel, 2, topic, (int)topic_len, NULL);

    int mention = 1;
    rc = sqlite3_step(sel);
    if (rc == SQLITE_ROW)
        mention = sqlite3_column_int(sel, 0) + 1;
    sqlite3_finalize(sel);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO topic_baselines(contact_id,topic,mention_count,last_mentioned) "
        "VALUES(?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_text(stmt, 2, topic, (int)topic_len, NULL);
    sqlite3_bind_int(stmt, 3, mention);
    sqlite3_bind_int64(stmt, 4, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_superhuman_topic_absence_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, int64_t now_ts, int64_t absence_days,
    char **out_json, size_t *out_len) {
    if (!sqlite_ctx || !alloc || !contact_id || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_json = NULL;
    *out_len = 0;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t cutoff = now_ts - (absence_days * 86400);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT topic, last_mentioned FROM topic_baselines WHERE contact_id=? "
        "AND (last_mentioned IS NULL OR last_mentioned < ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_int64(stmt, 2, cutoff);

    size_t buf_len = 0;
    size_t buf_cap = 0;
    char *buf = NULL;
    hu_error_t err = append_formatted(&buf, &buf_len, &buf_cap, alloc,
        "Topics absent >%lld days:\n", (long long)absence_days);
    if (err != HU_OK) {
        sqlite3_finalize(stmt);
        return err;
    }

    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *topic = (const char *)sqlite3_column_text(stmt, 0);
        int64_t last = sqlite3_column_int64(stmt, 1);
        const char *t = topic ? topic : "";
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc,
            "- %s (last: %lld)\n", t, (long long)last);
        if (err != HU_OK) {
            if (buf)
                alloc->free(alloc->ctx, buf, buf_cap);
            sqlite3_finalize(stmt);
            return err;
        }
        row_count++;
    }
    sqlite3_finalize(stmt);

    if (row_count == 0 && buf) {
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "(none)\n");
        if (err != HU_OK) {
            alloc->free(alloc->ctx, buf, buf_cap);
            return err;
        }
    }
    *out_json = buf;
    *out_len = buf_cap; /* allocated size for caller to free */
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Growth milestones
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_growth_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *topic, size_t topic_len,
    const char *before_state, size_t before_len, const char *after_state, size_t after_len) {
    (void)alloc;
    if (!sqlite_ctx || !contact_id || contact_id_len == 0 || !topic || topic_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO growth_milestones(contact_id,topic,before_state,after_state,created_at) "
        "VALUES(?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_text(stmt, 2, topic, (int)topic_len, NULL);
    sqlite3_bind_text(stmt, 3, before_state ? before_state : "", before_state ? (int)before_len : 0, NULL);
    sqlite3_bind_text(stmt, 4, after_state ? after_state : "", after_state ? (int)after_len : 0, NULL);
    sqlite3_bind_int64(stmt, 5, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_superhuman_growth_list_recent(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len) {
    if (!sqlite_ctx || !alloc || !contact_id || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_json = NULL;
    *out_len = 0;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT topic, before_state, after_state FROM growth_milestones WHERE contact_id=? "
        "ORDER BY created_at DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_int64(stmt, 2, (int64_t)(limit > 0 ? limit : 100));

    size_t buf_len = 0;
    size_t buf_cap = 0;
    char *buf = NULL;
    hu_error_t err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "Growth milestones:\n");
    if (err != HU_OK) {
        sqlite3_finalize(stmt);
        return err;
    }

    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *topic = (const char *)sqlite3_column_text(stmt, 0);
        const char *before = (const char *)sqlite3_column_text(stmt, 1);
        const char *after = (const char *)sqlite3_column_text(stmt, 2);
        const char *t = topic ? topic : "";
        const char *b = before ? before : "";
        const char *a = after ? after : "";
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc,
            "- %s: %s -> %s\n", t, b, a);
        if (err != HU_OK) {
            if (buf)
                alloc->free(alloc->ctx, buf, buf_cap);
            sqlite3_finalize(stmt);
            return err;
        }
        row_count++;
    }
    sqlite3_finalize(stmt);

    if (row_count == 0 && buf) {
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "(none)\n");
        if (err != HU_OK) {
            alloc->free(alloc->ctx, buf, buf_cap);
            return err;
        }
    }
    *out_json = buf;
    *out_len = buf_cap; /* allocated size for caller to free */
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Pattern observations
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_pattern_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len, const char *tone, size_t tone_len,
    int day_of_week, int hour) {
    if (!sqlite_ctx || !contact_id || contact_id_len == 0 || !topic || topic_len == 0 ||
        !tone || tone_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO pattern_observations(contact_id,topic,tone,day_of_week,hour,observed_at) "
        "VALUES(?,?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_text(stmt, 2, topic, (int)topic_len, NULL);
    sqlite3_bind_text(stmt, 3, tone, (int)tone_len, NULL);
    sqlite3_bind_int(stmt, 4, day_of_week);
    sqlite3_bind_int(stmt, 5, hour);
    sqlite3_bind_int64(stmt, 6, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_superhuman_pattern_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len) {
    if (!sqlite_ctx || !alloc || !contact_id || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_json = NULL;
    *out_len = 0;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT topic, tone, day_of_week, hour FROM pattern_observations WHERE contact_id=? "
        "ORDER BY observed_at DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, NULL);
    sqlite3_bind_int64(stmt, 2, (int64_t)(limit > 0 ? limit : 100));

    size_t buf_len = 0;
    size_t buf_cap = 0;
    char *buf = NULL;
    hu_error_t err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "Pattern observations:\n");
    if (err != HU_OK) {
        sqlite3_finalize(stmt);
        return err;
    }

    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *topic = (const char *)sqlite3_column_text(stmt, 0);
        const char *tone = (const char *)sqlite3_column_text(stmt, 1);
        int dow = sqlite3_column_int(stmt, 2);
        int h = sqlite3_column_int(stmt, 3);
        const char *t = topic ? topic : "";
        const char *tn = tone ? tone : "";
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc,
            "- %s | %s | dow=%d hour=%d\n", t, tn, dow, h);
        if (err != HU_OK) {
            if (buf)
                alloc->free(alloc->ctx, buf, buf_cap);
            sqlite3_finalize(stmt);
            return err;
        }
        row_count++;
    }
    sqlite3_finalize(stmt);

    if (row_count == 0 && buf) {
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "(none)\n");
        if (err != HU_OK) {
            alloc->free(alloc->ctx, buf, buf_cap);
            return err;
        }
    }
    *out_json = buf;
    *out_len = buf_cap; /* allocated size for caller to free */
    return HU_OK;
}

hu_error_t hu_superhuman_memory_build_context(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, bool include_avoidance,
    char **out, size_t *out_len) {
    if (!sqlite_ctx || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    sqlite3 *db = get_db(sqlite_ctx);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    size_t buf_len = 0;
    size_t buf_cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, buf_cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    buf[0] = '\0';

    hu_error_t err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "### Superhuman Memory\n\n");
    if (err != HU_OK) {
        alloc->free(alloc->ctx, buf, buf_cap);
        return err;
    }

    char *mm = NULL;
    size_t mm_len = 0;
    if (hu_superhuman_micro_moment_list(sqlite_ctx, alloc, contact_id, contact_id_len, 10,
                                        &mm, &mm_len) == HU_OK && mm && mm_len > 0 &&
        strstr(mm, "(none)") == NULL) {
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "%s\n", mm);
        alloc->free(alloc->ctx, mm, mm_len);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, buf, buf_cap);
            return err;
        }
    } else if (mm) {
        alloc->free(alloc->ctx, mm, mm_len);
    }

    hu_inside_joke_t *jokes = NULL;
    size_t jokes_count = 0;
    if (hu_superhuman_inside_joke_list(sqlite_ctx, alloc, contact_id, contact_id_len, 5,
                                       &jokes, &jokes_count) == HU_OK && jokes && jokes_count > 0) {
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "Inside jokes:\n");
        if (err != HU_OK) {
            hu_superhuman_inside_joke_free(alloc, jokes, jokes_count);
            alloc->free(alloc->ctx, buf, buf_cap);
            return err;
        }
        for (size_t i = 0; i < jokes_count; i++) {
            err = append_formatted(&buf, &buf_len, &buf_cap, alloc,
                "- %s | %s\n", jokes[i].context, jokes[i].punchline);
            if (err != HU_OK) {
                hu_superhuman_inside_joke_free(alloc, jokes, jokes_count);
                alloc->free(alloc->ctx, buf, buf_cap);
                return err;
            }
        }
        hu_superhuman_inside_joke_free(alloc, jokes, jokes_count);
    }

    char *growth = NULL;
    size_t growth_len = 0;
    if (hu_superhuman_growth_list_recent(sqlite_ctx, alloc, contact_id, contact_id_len, 5,
                                         &growth, &growth_len) == HU_OK && growth &&
        growth_len > 0 && strstr(growth, "(none)") == NULL) {
        err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "\n%s\n", growth);
        alloc->free(alloc->ctx, growth, growth_len);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, buf, buf_cap);
            return err;
        }
    } else if (growth) {
        alloc->free(alloc->ctx, growth, growth_len);
    }

    if (include_avoidance) {
        char *avoid = NULL;
        size_t avoid_len = 0;
        if (hu_superhuman_avoidance_list(sqlite_ctx, alloc, contact_id, contact_id_len,
                                         &avoid, &avoid_len) == HU_OK && avoid && avoid_len > 0 &&
            strstr(avoid, "(none)") == NULL) {
            err = append_formatted(&buf, &buf_len, &buf_cap, alloc, "\n%s\n", avoid);
            alloc->free(alloc->ctx, avoid, avoid_len);
            if (err != HU_OK) {
                alloc->free(alloc->ctx, buf, buf_cap);
                return err;
            }
        } else if (avoid) {
            alloc->free(alloc->ctx, avoid, avoid_len);
        }
    }

    if (buf_len <= sizeof("### Superhuman Memory\n\n") - 1) {
        alloc->free(alloc->ctx, buf, buf_cap);
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }

    *out = buf;
    *out_len = buf_len;
    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/superhuman.h"
#include <stddef.h>
#include <stdint.h>

hu_error_t hu_superhuman_inside_joke_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *context, size_t context_len,
    const char *punchline, size_t punchline_len) {
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)context; (void)context_len; (void)punchline; (void)punchline_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_inside_joke_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit,
    hu_inside_joke_t **out, size_t *out_count) {
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)limit; (void)out; (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_inside_joke_reference(void *sqlite_ctx, int64_t id) {
    (void)sqlite_ctx; (void)id;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_superhuman_inside_joke_free(hu_allocator_t *alloc, hu_inside_joke_t *arr, size_t count) {
    (void)alloc; (void)arr; (void)count;
}

hu_error_t hu_superhuman_commitment_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *description, size_t desc_len,
    const char *who, size_t who_len, int64_t deadline) {
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)description; (void)desc_len; (void)who; (void)who_len; (void)deadline;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_commitment_list_due(void *sqlite_ctx, hu_allocator_t *alloc,
    int64_t now_ts, size_t limit, hu_superhuman_commitment_t **out, size_t *out_count) {
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    (void)sqlite_ctx; (void)alloc; (void)now_ts; (void)limit; (void)out; (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_commitment_mark_followed_up(void *sqlite_ctx, int64_t id) {
    (void)sqlite_ctx; (void)id;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_superhuman_commitment_free(hu_allocator_t *alloc, hu_superhuman_commitment_t *arr,
    size_t count) {
    (void)alloc; (void)arr; (void)count;
}

hu_error_t hu_superhuman_temporal_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, int day_of_week, int hour, int64_t response_time_ms) {
    (void)sqlite_ctx; (void)contact_id; (void)contact_id_len;
    (void)day_of_week; (void)hour; (void)response_time_ms;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_temporal_get_quiet_hours(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, int *out_day, int *out_hour_start,
    int *out_hour_end) {
    if (out_day) *out_day = 0;
    if (out_hour_start) *out_hour_start = 0;
    if (out_hour_end) *out_hour_end = 1;
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)out_day; (void)out_hour_start; (void)out_hour_end;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_delayed_followup_schedule(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *topic, size_t topic_len,
    int64_t scheduled_at) {
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)topic; (void)topic_len; (void)scheduled_at;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_delayed_followup_list_due(void *sqlite_ctx, hu_allocator_t *alloc,
    int64_t now_ts, hu_delayed_followup_t **out, size_t *out_count) {
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    (void)sqlite_ctx; (void)alloc; (void)now_ts; (void)out; (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_delayed_followup_mark_sent(void *sqlite_ctx, int64_t id) {
    (void)sqlite_ctx; (void)id;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_superhuman_delayed_followup_free(hu_allocator_t *alloc, hu_delayed_followup_t *arr,
    size_t count) {
    (void)alloc; (void)arr; (void)count;
}

hu_error_t hu_superhuman_micro_moment_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *fact, size_t fact_len,
    const char *significance, size_t sig_len) {
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)fact; (void)fact_len; (void)significance; (void)sig_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_micro_moment_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len) {
    if (out_json) *out_json = NULL;
    if (out_len) *out_len = 0;
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)limit; (void)out_json; (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_avoidance_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len, bool topic_changed_quickly) {
    (void)sqlite_ctx; (void)contact_id; (void)contact_id_len;
    (void)topic; (void)topic_len; (void)topic_changed_quickly;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_avoidance_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, char **out_json, size_t *out_len) {
    if (out_json) *out_json = NULL;
    if (out_len) *out_len = 0;
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)out_json; (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_topic_baseline_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len) {
    (void)sqlite_ctx; (void)contact_id; (void)contact_id_len;
    (void)topic; (void)topic_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_topic_absence_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, int64_t now_ts, int64_t absence_days,
    char **out_json, size_t *out_len) {
    if (out_json) *out_json = NULL;
    if (out_len) *out_len = 0;
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)now_ts; (void)absence_days; (void)out_json; (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_growth_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *topic, size_t topic_len,
    const char *before_state, size_t before_len, const char *after_state, size_t after_len) {
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)topic; (void)topic_len; (void)before_state; (void)before_len;
    (void)after_state; (void)after_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_growth_list_recent(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len) {
    if (out_json) *out_json = NULL;
    if (out_len) *out_len = 0;
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)limit; (void)out_json; (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_pattern_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len, const char *tone, size_t tone_len,
    int day_of_week, int hour) {
    (void)sqlite_ctx; (void)contact_id; (void)contact_id_len;
    (void)topic; (void)topic_len; (void)tone; (void)tone_len;
    (void)day_of_week; (void)hour;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_pattern_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len) {
    if (out_json) *out_json = NULL;
    if (out_len) *out_len = 0;
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)limit; (void)out_json; (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_superhuman_memory_build_context(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, bool include_avoidance,
    char **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    (void)sqlite_ctx; (void)alloc; (void)contact_id; (void)contact_id_len;
    (void)include_avoidance; (void)out; (void)out_len;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
