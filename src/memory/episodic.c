typedef int hu_episodic_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/episodic.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_EPISODE_SALIENCE_BOOST 0.1
#define HU_EPISODE_SALIENCE_MAX    1.0

static void copy_episode_from_row(sqlite3_stmt *stmt, hu_episode_sqlite_t *e) {
    memset(e, 0, sizeof(*e));
    e->id = sqlite3_column_int64(stmt, 0);

    const char *cid = (const char *)sqlite3_column_text(stmt, 1);
    size_t cid_len = cid ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
    if (cid && cid_len > 0) {
        size_t copy = cid_len < sizeof(e->contact_id) - 1 ? cid_len : sizeof(e->contact_id) - 1;
        memcpy(e->contact_id, cid, copy);
        e->contact_id[copy] = '\0';
    }

    const char *sum = (const char *)sqlite3_column_text(stmt, 2);
    size_t sum_len = sum ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
    if (sum && sum_len > 0) {
        size_t copy = sum_len < sizeof(e->summary) - 1 ? sum_len : sizeof(e->summary) - 1;
        memcpy(e->summary, sum, copy);
        e->summary[copy] = '\0';
        e->summary_len = copy;
    }

    const char *arc = (const char *)sqlite3_column_text(stmt, 3);
    size_t arc_len = arc ? (size_t)sqlite3_column_bytes(stmt, 3) : 0;
    if (arc && arc_len > 0) {
        size_t copy = arc_len < sizeof(e->emotional_arc) - 1 ? arc_len : sizeof(e->emotional_arc) - 1;
        memcpy(e->emotional_arc, arc, copy);
        e->emotional_arc[copy] = '\0';
        e->emotional_arc_len = copy;
    }

    const char *km = (const char *)sqlite3_column_text(stmt, 4);
    size_t km_len = km ? (size_t)sqlite3_column_bytes(stmt, 4) : 0;
    if (km && km_len > 0) {
        size_t copy = km_len < sizeof(e->key_moments) - 1 ? km_len : sizeof(e->key_moments) - 1;
        memcpy(e->key_moments, km, copy);
        e->key_moments[copy] = '\0';
        e->key_moments_len = copy;
    }

    e->impact_score = sqlite3_column_double(stmt, 5);
    e->salience_score = sqlite3_column_double(stmt, 6);
    e->last_reinforced_at = sqlite3_column_int64(stmt, 7);

    const char *src = (const char *)sqlite3_column_text(stmt, 8);
    size_t src_len = src ? (size_t)sqlite3_column_bytes(stmt, 8) : 0;
    if (src && src_len > 0) {
        size_t copy = src_len < sizeof(e->source) - 1 ? src_len : sizeof(e->source) - 1;
        memcpy(e->source, src, copy);
        e->source[copy] = '\0';
        e->source_len = copy;
    }

    e->created_at = sqlite3_column_int64(stmt, 9);
}

hu_error_t hu_episode_store_insert(hu_allocator_t *alloc, void *db, const char *contact_id,
                                  size_t cid_len, const char *summary, size_t sum_len,
                                  const char *emotional_arc, size_t arc_len,
                                  const char *key_moments, size_t km_len, double impact_score,
                                  const char *source, size_t src_len, int64_t *out_id) {
    (void)alloc;
    if (!db || !contact_id || cid_len == 0 || !summary || sum_len == 0 || !out_id)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *sqlite_db = (sqlite3 *)db;
    const char *arc = emotional_arc && arc_len > 0 ? emotional_arc : NULL;
    size_t arc_use = arc ? arc_len : 0;
    const char *km = key_moments && km_len > 0 ? key_moments : NULL;
    size_t km_use = km ? km_len : 0;
    const char *src = source && src_len > 0 ? source : "conversation";
    size_t src_use = source && src_len > 0 ? src_len : 10;

    int64_t now_ts = (int64_t)time(NULL);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(sqlite_db,
                                "INSERT INTO episodes(contact_id,summary,emotional_arc,key_moments,"
                                "impact_score,salience_score,source,created_at) "
                                "VALUES(?,?,?,?,?,0.5,?,?)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, summary, (int)sum_len, SQLITE_STATIC);
    if (arc)
        sqlite3_bind_text(stmt, 3, arc, (int)arc_use, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    if (km)
        sqlite3_bind_text(stmt, 4, km, (int)km_use, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_double(stmt, 5, impact_score);
    sqlite3_bind_text(stmt, 6, src, (int)src_use, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 7, now_ts);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return HU_ERR_MEMORY_BACKEND;
    }
    *out_id = sqlite3_last_insert_rowid(sqlite_db);
    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_error_t hu_episode_get_by_contact(hu_allocator_t *alloc, void *db, const char *contact_id,
                                   size_t cid_len, size_t limit, int64_t since,
                                   hu_episode_sqlite_t **out, size_t *out_count) {
    if (!alloc || !db || !contact_id || cid_len == 0 || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sqlite3 *sqlite_db = (sqlite3 *)db;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(sqlite_db,
                                "SELECT id,contact_id,summary,emotional_arc,key_moments,"
                                "impact_score,salience_score,last_reinforced_at,source,created_at "
                                "FROM episodes WHERE contact_id=? AND created_at>=? "
                                "ORDER BY created_at DESC LIMIT ?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, since);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)limit);

    size_t cap = limit > 0 ? limit : 16;
    size_t count = 0;
    hu_episode_sqlite_t *arr =
        (hu_episode_sqlite_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_episode_sqlite_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            hu_episode_sqlite_t *n =
                (hu_episode_sqlite_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_episode_sqlite_t));
            if (!n) {
                alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_episode_sqlite_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(n, arr, count * sizeof(hu_episode_sqlite_t));
            alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_episode_sqlite_t));
            arr = n;
        }
        copy_episode_from_row(stmt, &arr[count]);
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0 && arr) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_episode_sqlite_t));
        arr = NULL;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_episode_associative_recall(hu_allocator_t *alloc, void *db, const char *query,
                                         size_t query_len, const char *contact_id,
                                         size_t cid_len, size_t limit,
                                         hu_episode_sqlite_t **out, size_t *out_count) {
    if (!alloc || !db || !query || query_len == 0 || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sqlite3 *sqlite_db = (sqlite3 *)db;

    /* Keyword matching: LIKE '%query%' on summary and key_moments. */
    char pattern_buf[512];
    if (query_len >= sizeof(pattern_buf) - 4)
        return HU_ERR_INVALID_ARGUMENT;
    pattern_buf[0] = '%';
    memcpy(pattern_buf + 1, query, query_len);
    pattern_buf[1 + query_len] = '%';
    pattern_buf[2 + query_len] = '\0';

    sqlite3_stmt *stmt = NULL;
    const char *sql = contact_id && cid_len > 0
        ? "SELECT id,contact_id,summary,emotional_arc,key_moments,impact_score,salience_score,"
          "last_reinforced_at,source,created_at FROM episodes "
          "WHERE (summary LIKE ? OR key_moments LIKE ?) AND contact_id=? "
          "ORDER BY created_at DESC LIMIT ?"
        : "SELECT id,contact_id,summary,emotional_arc,key_moments,impact_score,salience_score,"
          "last_reinforced_at,source,created_at FROM episodes "
          "WHERE summary LIKE ? OR key_moments LIKE ? "
          "ORDER BY created_at DESC LIMIT ?";
    int rc = sqlite3_prepare_v2(sqlite_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, pattern_buf, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, pattern_buf, -1, SQLITE_STATIC);
    if (contact_id && cid_len > 0) {
        sqlite3_bind_text(stmt, 3, contact_id, (int)cid_len, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, (sqlite3_int64)limit);
    } else {
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)limit);
    }

    size_t cap = limit > 0 ? limit : 16;
    size_t count = 0;
    hu_episode_sqlite_t *arr =
        (hu_episode_sqlite_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_episode_sqlite_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            hu_episode_sqlite_t *n =
                (hu_episode_sqlite_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_episode_sqlite_t));
            if (!n) {
                alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_episode_sqlite_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(n, arr, count * sizeof(hu_episode_sqlite_t));
            alloc->free(alloc->ctx, arr, (cap / 2) * sizeof(hu_episode_sqlite_t));
            arr = n;
        }
        copy_episode_from_row(stmt, &arr[count]);
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0 && arr) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_episode_sqlite_t));
        arr = NULL;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_episode_reinforce(void *db, int64_t episode_id, int64_t now_ts) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *sqlite_db = (sqlite3 *)db;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(sqlite_db,
                                "UPDATE episodes SET salience_score=MIN(1.0,salience_score+?), "
                                "last_reinforced_at=? WHERE id=?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_double(stmt, 1, HU_EPISODE_SALIENCE_BOOST);
    sqlite3_bind_int64(stmt, 2, now_ts);
    sqlite3_bind_int64(stmt, 3, episode_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

void hu_episode_free(hu_allocator_t *alloc, hu_episode_sqlite_t *episodes, size_t count) {
    if (!alloc || !episodes || count == 0)
        return;
    alloc->free(alloc->ctx, episodes, count * sizeof(hu_episode_sqlite_t));
}

#endif /* HU_ENABLE_SQLITE */
