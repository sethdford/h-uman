typedef int hu_emotional_residue_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/emotional_residue.h"
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define INTENSITY_THRESHOLD 0.05
#define SEC_PER_DAY 86400.0

hu_error_t hu_emotional_residue_add(sqlite3 *db, int64_t episode_id, const char *contact_id,
                                    size_t cid_len, double valence, double intensity,
                                    double decay_rate, int64_t *out_id) {
    if (!db || !contact_id || cid_len == 0 || !out_id)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO emotional_residue(episode_id,contact_id,valence,"
                                "intensity,decay_rate,created_at) VALUES(?,?,?,?,?,?)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    int64_t now_ts = (int64_t)time(NULL);
    if (episode_id != 0)
        sqlite3_bind_int64(stmt, 1, episode_id);
    else
        sqlite3_bind_null(stmt, 1);
    sqlite3_bind_text(stmt, 2, contact_id, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, valence);
    sqlite3_bind_double(stmt, 4, intensity);
    sqlite3_bind_double(stmt, 5, decay_rate);
    sqlite3_bind_int64(stmt, 6, now_ts);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return HU_ERR_MEMORY_BACKEND;
    }
    *out_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_error_t hu_emotional_residue_get_active(hu_allocator_t *alloc, sqlite3 *db,
                                           const char *contact_id, size_t cid_len, int64_t now_ts,
                                           hu_emotional_residue_t **out, size_t *out_count) {
    if (!alloc || !db || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                               "SELECT id,episode_id,contact_id,valence,intensity,decay_rate,"
                               "created_at FROM emotional_residue WHERE contact_id=?",
                               -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);

    size_t cap = 16;
    hu_emotional_residue_t *arr =
        (hu_emotional_residue_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_emotional_residue_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, cap * sizeof(hu_emotional_residue_t));
    size_t n = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t created_at = sqlite3_column_int64(stmt, 6);
        double decay_rate = sqlite3_column_double(stmt, 5);
        double intensity = sqlite3_column_double(stmt, 4);
        double days = (double)(now_ts - created_at) / SEC_PER_DAY;
        double intensity_current = intensity * exp(-decay_rate * days);
        if (intensity_current < INTENSITY_THRESHOLD)
            continue;

        if (n >= cap) {
            size_t new_cap = cap * 2;
            hu_emotional_residue_t *nb =
                (hu_emotional_residue_t *)alloc->realloc(alloc->ctx, arr,
                                                         cap * sizeof(hu_emotional_residue_t),
                                                         new_cap * sizeof(hu_emotional_residue_t));
            if (!nb) {
                alloc->free(alloc->ctx, arr, cap * sizeof(hu_emotional_residue_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            arr = nb;
            cap = new_cap;
        }
        hu_emotional_residue_t *e = &arr[n];
        e->id = sqlite3_column_int64(stmt, 0);
        e->episode_id = sqlite3_column_int64(stmt, 1);
        const char *cid = (const char *)sqlite3_column_text(stmt, 2);
        if (cid) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 2);
            memcpy(e->contact_id, cid, MIN(len, sizeof(e->contact_id) - 1));
            e->contact_id[MIN(len, sizeof(e->contact_id) - 1)] = '\0';
        }
        e->valence = sqlite3_column_double(stmt, 3);
        e->intensity = intensity_current;
        e->decay_rate = decay_rate;
        e->created_at = created_at;
        n++;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_emotional_residue_t));
        return HU_ERR_MEMORY_BACKEND;
    }

    if (n == 0) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_emotional_residue_t));
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }
    *out = arr;
    *out_count = n;
    return HU_OK;
}

char *hu_emotional_residue_build_directive(hu_allocator_t *alloc,
                                           const hu_emotional_residue_t *residues, size_t count,
                                           size_t *out_len) {
    if (!alloc || !residues || count == 0 || !out_len)
        return NULL;
    *out_len = 0;

    const char *sentiment = residues[0].valence >= 0.0 ? "positive" : "negative";
    const char *contact = residues[0].contact_id[0] ? residues[0].contact_id : "contact";
    double intensity = residues[0].intensity;

    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp),
                    "[EMOTIONAL RESIDUE: Recent interaction with [%s] left [%s] weight ([%.2f]). "
                    "Match warmth/caution accordingly.]",
                    contact, sentiment, intensity);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return NULL;

    size_t need = (size_t)n + 1;
    char *buf = (char *)alloc->alloc(alloc->ctx, need);
    if (!buf)
        return NULL;
    memcpy(buf, tmp, need);
    *out_len = (size_t)n;
    return buf;
}

#else /* !HU_ENABLE_SQLITE */

#include "human/core/error.h"
#include "human/memory/emotional_residue.h"

hu_error_t hu_emotional_residue_add(void *db, int64_t episode_id, const char *contact_id,
                                    size_t cid_len, double valence, double intensity,
                                    double decay_rate, int64_t *out_id) {
    (void)db;
    (void)episode_id;
    (void)contact_id;
    (void)cid_len;
    (void)valence;
    (void)intensity;
    (void)decay_rate;
    (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_emotional_residue_get_active(hu_allocator_t *alloc, void *db,
                                           const char *contact_id, size_t cid_len, int64_t now_ts,
                                           hu_emotional_residue_t **out, size_t *out_count) {
    (void)alloc;
    (void)db;
    (void)contact_id;
    (void)cid_len;
    (void)now_ts;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

char *hu_emotional_residue_build_directive(hu_allocator_t *alloc,
                                           const hu_emotional_residue_t *residues, size_t count,
                                           size_t *out_len) {
    (void)alloc;
    (void)residues;
    (void)count;
    (void)out_len;
    return NULL;
}

#endif /* HU_ENABLE_SQLITE */
