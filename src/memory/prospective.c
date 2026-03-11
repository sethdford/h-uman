typedef int hu_prospective_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/prospective.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

hu_error_t hu_prospective_store(sqlite3 *db, const char *trigger_type, size_t tt_len,
                                const char *trigger_value, size_t tv_len,
                                const char *action, size_t action_len,
                                const char *contact_id, size_t cid_len, int64_t expires_at,
                                int64_t *out_id) {
    if (!db || !trigger_type || !trigger_value || !action || !out_id)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                               "INSERT INTO prospective_memories(trigger_type,trigger_value,action,"
                               "contact_id,expires_at,created_at) VALUES(?,?,?,?,?,?)",
                               -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    int64_t now_ts = (int64_t)time(NULL);
    sqlite3_bind_text(stmt, 1, trigger_type, (int)tt_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, trigger_value, (int)tv_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, action, (int)action_len, SQLITE_STATIC);
    if (contact_id && cid_len > 0)
        sqlite3_bind_text(stmt, 4, contact_id, (int)cid_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_int64(stmt, 5, expires_at);
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
    int rc = sqlite3_prepare_v2(db,
                               "SELECT id,trigger_type,trigger_value,action,contact_id,expires_at,"
                               "created_at FROM prospective_memories WHERE fired=0 AND "
                               "trigger_type=? AND (instr(?,trigger_value)>0) AND "
                               "(contact_id IS NULL OR contact_id=?) AND "
                               "(expires_at IS NULL OR expires_at=0 OR expires_at>?)",
                               -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, trigger_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, trigger_value, (int)tv_len, SQLITE_STATIC);
    if (contact_id && cid_len > 0)
        sqlite3_bind_text(stmt, 3, contact_id, (int)cid_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int64(stmt, 4, now_ts);

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
            hu_prospective_entry_t *nb =
                (hu_prospective_entry_t *)alloc->realloc(alloc->ctx, arr,
                                                         cap * sizeof(hu_prospective_entry_t),
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

hu_error_t hu_prospective_mark_fired(sqlite3 *db, int64_t id) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE prospective_memories SET fired=1 WHERE id=?",
                               -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

#else /* !HU_ENABLE_SQLITE */

#include "human/core/error.h"
#include "human/memory/prospective.h"

hu_error_t hu_prospective_store(void *db, const char *trigger_type, size_t tt_len,
                                const char *trigger_value, size_t tv_len,
                                const char *action, size_t action_len,
                                const char *contact_id, size_t cid_len, int64_t expires_at,
                                int64_t *out_id) {
    (void)db;
    (void)trigger_type;
    (void)tt_len;
    (void)trigger_value;
    (void)tv_len;
    (void)action;
    (void)action_len;
    (void)contact_id;
    (void)cid_len;
    (void)expires_at;
    (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_prospective_check_triggers(hu_allocator_t *alloc, void *db,
                                        const char *trigger_type, const char *trigger_value,
                                        size_t tv_len, const char *contact_id, size_t cid_len,
                                        int64_t now_ts, hu_prospective_entry_t **out,
                                        size_t *out_count) {
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

hu_error_t hu_prospective_mark_fired(void *db, int64_t id) {
    (void)db;
    (void)id;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */
