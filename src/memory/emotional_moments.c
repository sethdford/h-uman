#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/emotional_moments.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_EMOTIONAL_MOMENT_7_DAYS_SEC  (7 * 24 * 3600)
#define HU_EMOTIONAL_MOMENT_MIN_OFFSET  86400   /* 1 day */
#define HU_EMOTIONAL_MOMENT_RANGE       172800  /* 2 days: 259200 - 86400 */

static int64_t compute_follow_up_offset(void) {
#ifdef HU_IS_TEST
    (void)0;
    return (int64_t)HU_EMOTIONAL_MOMENT_MIN_OFFSET;
#else
    return (int64_t)HU_EMOTIONAL_MOMENT_MIN_OFFSET +
           (int64_t)(rand() % (HU_EMOTIONAL_MOMENT_RANGE + 1));
#endif
}

hu_error_t hu_emotional_moment_record(hu_allocator_t *alloc, hu_memory_t *memory,
                                      const char *contact_id, size_t contact_id_len,
                                      const char *topic, size_t topic_len,
                                      const char *emotion, size_t emotion_len, float intensity) {
    (void)alloc;
    if (!memory || !contact_id || contact_id_len == 0 || !topic || topic_len == 0 ||
        !emotion || emotion_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    int64_t cutoff = now_ts - (int64_t)HU_EMOTIONAL_MOMENT_7_DAYS_SEC;

    /* Check duplicate: same contact_id + topic within last 7 days with followed_up=0 */
    sqlite3_stmt *check = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT 1 FROM emotional_moments WHERE contact_id=? AND topic=? "
                                "AND followed_up=0 AND created_at>? LIMIT 1",
                                -1, &check, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(check, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(check, 2, topic, (int)topic_len, SQLITE_STATIC);
    sqlite3_bind_int64(check, 3, cutoff);
    rc = sqlite3_step(check);
    bool duplicate = (rc == SQLITE_ROW);
    sqlite3_finalize(check);
    if (duplicate)
        return HU_OK; /* Skip, no error */

    int64_t offset = compute_follow_up_offset();
    int64_t follow_up = now_ts + offset;

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
                            "INSERT INTO emotional_moments(contact_id,topic,emotion,intensity,"
                            "created_at,follow_up_date,followed_up) VALUES(?,?,?,?,?,?,0)",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, topic, (int)topic_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, emotion, (int)emotion_len, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, (double)intensity);
    sqlite3_bind_int64(stmt, 5, now_ts);
    sqlite3_bind_int64(stmt, 6, follow_up);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_emotional_moment_get_due(hu_allocator_t *alloc, hu_memory_t *memory,
                                       int64_t now_ts, hu_emotional_moment_t **out,
                                       size_t *out_count) {
    if (!alloc || !memory || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT id,contact_id,topic,emotion,intensity,created_at,"
                                "follow_up_date,followed_up FROM emotional_moments "
                                "WHERE follow_up_date<=? AND followed_up=0 ORDER BY follow_up_date",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, now_ts);

    size_t cap = 16;
    size_t count = 0;
    hu_emotional_moment_t *arr =
        (hu_emotional_moment_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_emotional_moment_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            hu_emotional_moment_t *n =
                (hu_emotional_moment_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_emotional_moment_t));
            if (!n) {
                for (size_t i = 0; i < count; i++)
                    /* no heap fields to free in hu_emotional_moment_t */
                    (void)i;
                alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_emotional_moment_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(n, arr, count * sizeof(hu_emotional_moment_t));
            alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_emotional_moment_t));
            arr = n;
        }

        hu_emotional_moment_t *e = &arr[count];
        memset(e, 0, sizeof(*e));
        e->id = sqlite3_column_int64(stmt, 0);
        const char *cid = (const char *)sqlite3_column_text(stmt, 1);
        size_t cid_len = cid ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
        if (cid && cid_len > 0) {
            size_t copy = cid_len < sizeof(e->contact_id) - 1 ? cid_len : sizeof(e->contact_id) - 1;
            memcpy(e->contact_id, cid, copy);
            e->contact_id[copy] = '\0';
        }
        const char *top = (const char *)sqlite3_column_text(stmt, 2);
        if (top) {
            size_t top_len = strlen(top);
            size_t copy = top_len < sizeof(e->topic) - 1 ? top_len : sizeof(e->topic) - 1;
            memcpy(e->topic, top, copy);
            e->topic[copy] = '\0';
        }
        const char *emo = (const char *)sqlite3_column_text(stmt, 3);
        size_t emo_len = emo ? (size_t)sqlite3_column_bytes(stmt, 3) : 0;
        if (emo && emo_len > 0) {
            size_t copy = emo_len < sizeof(e->emotion) - 1 ? emo_len : sizeof(e->emotion) - 1;
            memcpy(e->emotion, emo, copy);
            e->emotion[copy] = '\0';
        }
        e->intensity = (float)sqlite3_column_double(stmt, 4);
        e->created_at = sqlite3_column_int64(stmt, 5);
        e->follow_up_date = sqlite3_column_int64(stmt, 6);
        e->followed_up = sqlite3_column_int(stmt, 7) != 0;
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0 && arr) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_emotional_moment_t));
        arr = NULL;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_emotional_moment_mark_followed_up(hu_memory_t *memory, int64_t id) {
    if (!memory)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE emotional_moments SET followed_up=1 WHERE id=?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

#else /* !HU_ENABLE_SQLITE */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/emotional_moments.h"

hu_error_t hu_emotional_moment_record(hu_allocator_t *alloc, hu_memory_t *memory,
                                      const char *contact_id, size_t contact_id_len,
                                      const char *topic, size_t topic_len,
                                      const char *emotion, size_t emotion_len, float intensity) {
    (void)alloc;
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    (void)topic;
    (void)topic_len;
    (void)emotion;
    (void)emotion_len;
    (void)intensity;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_emotional_moment_get_due(hu_allocator_t *alloc, hu_memory_t *memory,
                                       int64_t now_ts, hu_emotional_moment_t **out,
                                       size_t *out_count) {
    (void)alloc;
    (void)memory;
    (void)now_ts;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_emotional_moment_mark_followed_up(hu_memory_t *memory, int64_t id) {
    (void)memory;
    (void)id;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */
