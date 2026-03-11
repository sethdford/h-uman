typedef int hu_opinions_unused_; /* ISO C requires non-empty translation unit */

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/opinions.h"
#include <ctype.h>
#include <sqlite3.h>
#include <string.h>

static bool position_matches(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    return (a_len == 0 && b_len == 0) ||
           (a && b && memcmp(a, b, a_len) == 0);
}

hu_error_t hu_opinions_upsert(hu_allocator_t *alloc, hu_memory_t *memory,
                             const char *topic, size_t topic_len,
                             const char *position, size_t position_len,
                             float confidence, int64_t now_ts) {
    if (!alloc || !memory)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    /* Find existing non-superseded opinion on topic */
    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT id, position FROM opinions WHERE topic=? AND "
                                "superseded_by IS NULL LIMIT 1",
                                -1, &sel, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(sel, 1, topic, (int)topic_len, SQLITE_STATIC);
    rc = sqlite3_step(sel);

    if (rc == SQLITE_ROW) {
        int64_t old_id = sqlite3_column_int64(sel, 0);
        const char *old_pos = (const char *)sqlite3_column_text(sel, 1);
        size_t old_pos_len = old_pos ? (size_t)sqlite3_column_bytes(sel, 1) : 0;
        bool same_position = position_matches(position, position_len, old_pos, old_pos_len);
        sqlite3_finalize(sel);

        if (same_position) {
            /* Same position: update last_expressed, confidence */
            sqlite3_stmt *upd = NULL;
            rc = sqlite3_prepare_v2(db,
                                    "UPDATE opinions SET last_expressed=?, confidence=? WHERE id=?",
                                    -1, &upd, NULL);
            if (rc != SQLITE_OK)
                return HU_ERR_MEMORY_BACKEND;
            sqlite3_bind_int64(upd, 1, now_ts);
            sqlite3_bind_double(upd, 2, (double)confidence);
            sqlite3_bind_int64(upd, 3, old_id);
            rc = sqlite3_step(upd);
            sqlite3_finalize(upd);
            return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
        }

        /* Different position: insert new, then supersede old */
        sqlite3_stmt *ins = NULL;
        rc = sqlite3_prepare_v2(db,
                                "INSERT INTO opinions(topic,position,confidence,first_expressed,"
                                "last_expressed,superseded_by) VALUES(?,?,?,?,?,NULL)",
                                -1, &ins, NULL);
        if (rc != SQLITE_OK)
            return HU_ERR_MEMORY_BACKEND;
        sqlite3_bind_text(ins, 1, topic, (int)topic_len, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, position, (int)position_len, SQLITE_STATIC);
        sqlite3_bind_double(ins, 3, (double)confidence);
        sqlite3_bind_int64(ins, 4, now_ts);
        sqlite3_bind_int64(ins, 5, now_ts);
        rc = sqlite3_step(ins);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(ins);
            return HU_ERR_MEMORY_BACKEND;
        }
        int64_t new_id = sqlite3_last_insert_rowid(db);
        sqlite3_finalize(ins);

        sqlite3_stmt *sup = NULL;
        rc = sqlite3_prepare_v2(db, "UPDATE opinions SET superseded_by=? WHERE id=?",
                                -1, &sup, NULL);
        if (rc != SQLITE_OK)
            return HU_ERR_MEMORY_BACKEND;
        sqlite3_bind_int64(sup, 1, new_id);
        sqlite3_bind_int64(sup, 2, old_id);
        rc = sqlite3_step(sup);
        sqlite3_finalize(sup);
        return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
    }

    sqlite3_finalize(sel);

    /* No existing: insert new */
    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(db,
                            "INSERT INTO opinions(topic,position,confidence,first_expressed,"
                            "last_expressed,superseded_by) VALUES(?,?,?,?,?,NULL)",
                            -1, &ins, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(ins, 1, topic, (int)topic_len, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, position, (int)position_len, SQLITE_STATIC);
    sqlite3_bind_double(ins, 3, (double)confidence);
    sqlite3_bind_int64(ins, 4, now_ts);
    sqlite3_bind_int64(ins, 5, now_ts);
    rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static hu_error_t opinions_query(hu_allocator_t *alloc, hu_memory_t *memory,
                                 const char *topic, size_t topic_len,
                                 bool superseded_only,
                                 hu_opinion_t **out, size_t *out_count) {
    if (!alloc || !memory || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    const char *sql = superseded_only
                          ? "SELECT id,topic,position,confidence,first_expressed,last_expressed,"
                            "superseded_by FROM opinions WHERE topic=? AND superseded_by IS NOT NULL"
                          : "SELECT id,topic,position,confidence,first_expressed,last_expressed,"
                            "superseded_by FROM opinions WHERE topic=? AND superseded_by IS NULL";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, topic, (int)topic_len, SQLITE_STATIC);

    size_t cap = 16;
    size_t count = 0;
    hu_opinion_t *arr =
        (hu_opinion_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_opinion_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t old_cap = cap;
            cap *= 2;
            hu_opinion_t *n =
                (hu_opinion_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_opinion_t));
            if (!n) {
                hu_opinions_free(alloc, arr, count);
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(n, arr, count * sizeof(hu_opinion_t));
            alloc->free(alloc->ctx, arr, old_cap * sizeof(hu_opinion_t));
            arr = n;
        }

        hu_opinion_t *o = &arr[count];
        memset(o, 0, sizeof(*o));
        o->id = sqlite3_column_int64(stmt, 0);
        const char *t = (const char *)sqlite3_column_text(stmt, 1);
        size_t t_len = t ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
        if (t && t_len > 0) {
            o->topic = hu_strndup(alloc, t, t_len);
            o->topic_len = t_len;
        }
        const char *p = (const char *)sqlite3_column_text(stmt, 2);
        size_t p_len = p ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
        if (p && p_len > 0) {
            o->position = hu_strndup(alloc, p, p_len);
            o->position_len = p_len;
        }
        o->confidence = (float)sqlite3_column_double(stmt, 3);
        o->first_expressed = sqlite3_column_int64(stmt, 4);
        o->last_expressed = sqlite3_column_int64(stmt, 5);
        o->superseded_by = sqlite3_column_int64(stmt, 6);
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0 && arr) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_opinion_t));
        arr = NULL;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_opinions_get(hu_allocator_t *alloc, hu_memory_t *memory,
                          const char *topic, size_t topic_len,
                          hu_opinion_t **out, size_t *out_count) {
    return opinions_query(alloc, memory, topic, topic_len, false, out, out_count);
}

hu_error_t hu_opinions_get_superseded(hu_allocator_t *alloc, hu_memory_t *memory,
                                     const char *topic, size_t topic_len,
                                     hu_opinion_t **out, size_t *out_count) {
    return opinions_query(alloc, memory, topic, topic_len, true, out, out_count);
}

bool hu_opinions_is_core_value(const char *topic, size_t topic_len,
                               const char *const *core_values, size_t count) {
    if (!topic || !core_values || count == 0)
        return false;
    for (size_t i = 0; i < count; i++) {
        const char *cv = core_values[i];
        if (!cv)
            continue;
        size_t cv_len = strlen(cv);
        if (topic_len != cv_len)
            continue;
        bool match = true;
        for (size_t j = 0; j < topic_len; j++) {
            if (tolower((unsigned char)topic[j]) != tolower((unsigned char)cv[j])) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

void hu_opinions_free(hu_allocator_t *alloc, hu_opinion_t *ops, size_t count) {
    if (!alloc || !ops)
        return;
    for (size_t i = 0; i < count; i++) {
        if (ops[i].topic)
            hu_str_free(alloc, ops[i].topic);
        if (ops[i].position)
            hu_str_free(alloc, ops[i].position);
    }
    alloc->free(alloc->ctx, ops, count * sizeof(hu_opinion_t));
}

#endif /* HU_ENABLE_SQLITE */
