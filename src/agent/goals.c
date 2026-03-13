#ifdef HU_ENABLE_SQLITE

#include "human/agent/goals.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hu_error_t hu_goal_engine_create(hu_allocator_t *alloc, sqlite3 *db,
                                 hu_goal_engine_t *out) {
    if (!alloc || !db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->alloc = alloc;
    out->db = db;
    return HU_OK;
}

void hu_goal_engine_deinit(hu_goal_engine_t *engine) {
    (void)engine;
}

hu_error_t hu_goal_init_tables(hu_goal_engine_t *engine) {
    if (!engine || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS goals("
        "id INTEGER PRIMARY KEY, description TEXT, status INTEGER DEFAULT 0, "
        "priority REAL DEFAULT 0.5, progress REAL DEFAULT 0.0, parent_id INTEGER DEFAULT 0, "
        "created_at INTEGER, updated_at INTEGER, deadline INTEGER DEFAULT 0)";

    char *err = NULL;
    int rc = sqlite3_exec(engine->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            sqlite3_free(err);
        }
        return HU_ERR_MEMORY_BACKEND;
    }
    return HU_OK;
}

hu_error_t hu_goal_create(hu_goal_engine_t *engine,
                          const char *description, size_t desc_len,
                          double priority, int64_t parent_id,
                          int64_t deadline, int64_t now_ts,
                          int64_t *out_id) {
    if (!engine || !engine->db || !out_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (!description || desc_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                                "INSERT INTO goals(description, status, priority, progress, "
                                "parent_id, created_at, updated_at, deadline) "
                                "VALUES(?, 0, ?, 0.0, ?, ?, ?, ?)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, description, (int)desc_len, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, priority);
    sqlite3_bind_int64(stmt, 3, parent_id);
    sqlite3_bind_int64(stmt, 4, now_ts);
    sqlite3_bind_int64(stmt, 5, now_ts);
    sqlite3_bind_int64(stmt, 6, deadline);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;

    *out_id = sqlite3_last_insert_rowid(engine->db);
    return HU_OK;
}

hu_error_t hu_goal_update_status(hu_goal_engine_t *engine, int64_t goal_id,
                                 hu_auto_goal_status_t status, int64_t now_ts) {
    if (!engine || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                                "UPDATE goals SET status=?, updated_at=? WHERE id=?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int(stmt, 1, (int)status);
    sqlite3_bind_int64(stmt, 2, now_ts);
    sqlite3_bind_int64(stmt, 3, goal_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_goal_update_progress(hu_goal_engine_t *engine, int64_t goal_id,
                                   double progress, int64_t now_ts) {
    if (!engine || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    hu_auto_goal_status_t new_status = (progress >= 1.0) ? HU_AUTO_GOAL_COMPLETED : HU_AUTO_GOAL_ACTIVE;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                                "UPDATE goals SET progress=?, updated_at=?, status=? WHERE id=?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_double(stmt, 1, progress);
    sqlite3_bind_int64(stmt, 2, now_ts);
    sqlite3_bind_int(stmt, 3, (int)new_status);
    sqlite3_bind_int64(stmt, 4, goal_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static void copy_str_to_field(char *dst, size_t cap, const char *src, size_t src_len,
                              size_t *out_len) {
    if (!dst || cap == 0) {
        if (out_len)
            *out_len = 0;
        return;
    }
    size_t len = src_len;
    if (len >= cap)
        len = cap - 1;
    if (src && len > 0)
        memcpy(dst, src, len);
    dst[len] = '\0';
    if (out_len)
        *out_len = len;
}

static hu_error_t load_goal_from_row(sqlite3_stmt *stmt, hu_goal_t *out) {
    memset(out, 0, sizeof(*out));
    out->id = sqlite3_column_int64(stmt, 0);

    const char *c = (const char *)sqlite3_column_text(stmt, 1);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 1);
        copy_str_to_field(out->description, sizeof(out->description), c, len, &out->description_len);
    }

    out->status = (hu_auto_goal_status_t)sqlite3_column_int(stmt, 2);
    out->priority = sqlite3_column_double(stmt, 3);
    out->progress = sqlite3_column_double(stmt, 4);
    out->parent_id = sqlite3_column_int64(stmt, 5);
    out->created_at = sqlite3_column_int64(stmt, 6);
    out->updated_at = sqlite3_column_int64(stmt, 7);
    out->deadline = sqlite3_column_int64(stmt, 8);
    return HU_OK;
}

hu_error_t hu_goal_decompose(hu_goal_engine_t *engine, int64_t parent_id,
                             const char **descriptions, const size_t *desc_lens,
                             size_t count, int64_t now_ts,
                             int64_t *out_ids) {
    if (!engine || !engine->db || !descriptions || !desc_lens || !out_ids || count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    double parent_priority = 0.5;
    if (parent_id > 0) {
        hu_goal_t parent = {0};
        bool found = false;
        hu_error_t err = hu_goal_get(engine, parent_id, &parent, &found);
        if (err != HU_OK)
            return err;
        if (found)
            parent_priority = parent.priority;
    }

    double sub_priority = (count > 0) ? (parent_priority / (double)count) : 0.5;
    if (sub_priority > 1.0)
        sub_priority = 1.0;
    if (sub_priority < 0.0)
        sub_priority = 0.0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                                "INSERT INTO goals(description, status, priority, progress, "
                                "parent_id, created_at, updated_at, deadline) "
                                "VALUES(?, 0, ?, 0.0, ?, ?, ?, 0)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    for (size_t i = 0; i < count; i++) {
        const char *desc = descriptions[i];
        size_t dlen = desc_lens[i];
        if (!desc)
            dlen = 0;

        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, desc ? desc : "", (int)dlen, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 2, sub_priority);
        sqlite3_bind_int64(stmt, 3, parent_id);
        sqlite3_bind_int64(stmt, 4, now_ts);
        sqlite3_bind_int64(stmt, 5, now_ts);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return HU_ERR_MEMORY_BACKEND;
        }
        out_ids[i] = sqlite3_last_insert_rowid(engine->db);
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_error_t hu_goal_select_next(hu_goal_engine_t *engine, hu_goal_t *out,
                               bool *found) {
    if (!engine || !engine->db || !out || !found)
        return HU_ERR_INVALID_ARGUMENT;

    *found = false;
    memset(out, 0, sizeof(*out));

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                               "SELECT id, description, status, priority, progress, "
                               "parent_id, created_at, updated_at, deadline "
                               "FROM goals WHERE status IN (0, 1) "
                               "ORDER BY priority DESC, created_at ASC LIMIT 1",
                               -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        load_goal_from_row(stmt, out);
        *found = true;
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_error_t hu_goal_list_active(hu_goal_engine_t *engine,
                               hu_goal_t **out, size_t *out_count) {
    if (!engine || !engine->db || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                                "SELECT id, description, status, priority, progress, "
                                "parent_id, created_at, updated_at, deadline "
                                "FROM goals WHERE status IN (0, 1) ORDER BY priority DESC",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    size_t cap = 0;
    size_t n = 0;
    hu_goal_t *arr = NULL;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            hu_goal_t *new_arr = (hu_goal_t *)engine->alloc->realloc(
                engine->alloc->ctx, arr,
                cap * sizeof(hu_goal_t),
                new_cap * sizeof(hu_goal_t));
            if (!new_arr) {
                sqlite3_finalize(stmt);
                hu_goal_free(engine->alloc, arr, n);
                return HU_ERR_OUT_OF_MEMORY;
            }
            arr = new_arr;
            cap = new_cap;
        }
        load_goal_from_row(stmt, &arr[n]);
        n++;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        return HU_ERR_MEMORY_BACKEND;

    *out = arr;
    *out_count = n;
    return HU_OK;
}

hu_error_t hu_goal_get(hu_goal_engine_t *engine, int64_t goal_id,
                       hu_goal_t *out, bool *found) {
    if (!engine || !engine->db || !out || !found)
        return HU_ERR_INVALID_ARGUMENT;

    *found = false;
    memset(out, 0, sizeof(*out));

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                                "SELECT id, description, status, priority, progress, "
                                "parent_id, created_at, updated_at, deadline "
                                "FROM goals WHERE id=?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, goal_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        load_goal_from_row(stmt, out);
        *found = true;
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_error_t hu_goal_count(hu_goal_engine_t *engine, size_t *out) {
    if (!engine || !engine->db || !out)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, "SELECT COUNT(*) FROM goals", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    *out = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_error_t hu_goal_build_context(hu_goal_engine_t *engine,
                                 char **out, size_t *out_len) {
    if (!engine || !engine->db || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    hu_goal_t *goals = NULL;
    size_t count = 0;
    hu_error_t err = hu_goal_list_active(engine, &goals, &count);
    if (err != HU_OK)
        return err;

    size_t max_show = count > 5 ? 5 : count;
    if (max_show == 0) {
        *out = hu_strndup(engine->alloc, "Active goals:\n(no active goals)", 32);
        if (!*out) {
            hu_goal_free(engine->alloc, goals, count);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out_len = strlen(*out);
        hu_goal_free(engine->alloc, goals, count);
        return HU_OK;
    }

    size_t est = 64;
    for (size_t i = 0; i < max_show; i++) {
        est += 8 + 32 + goals[i].description_len + 16;
    }
    char *buf = (char *)engine->alloc->alloc(engine->alloc->ctx, est);
    if (!buf) {
        hu_goal_free(engine->alloc, goals, count);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t pos = 0;
    int n = snprintf(buf + pos, est - pos, "Active goals:\n");
    if (n > 0 && (size_t)n < est - pos)
        pos += (size_t)n;

    for (size_t i = 0; i < max_show && pos < est - 1; i++) {
        int pct = (int)(goals[i].progress * 100.0);
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;

        n = snprintf(buf + pos, est - pos, "%zu. [%.2f] %.*s (%d%%)\n",
                     i + 1, goals[i].priority,
                     (int)goals[i].description_len, goals[i].description,
                     pct);
        if (n > 0 && (size_t)n < est - pos)
            pos += (size_t)n;
    }

    hu_goal_free(engine->alloc, goals, count);
    buf[pos] = '\0';

    if (pos + 1 < est) {
        char *trimmed = (char *)engine->alloc->realloc(engine->alloc->ctx, buf, est, pos + 1);
        if (trimmed)
            buf = trimmed;
    }

    *out = buf;
    *out_len = pos;
    return HU_OK;
}

const char *hu_auto_goal_status_str(hu_auto_goal_status_t status) {
    switch (status) {
    case HU_AUTO_GOAL_PENDING:
        return "pending";
    case HU_AUTO_GOAL_ACTIVE:
        return "active";
    case HU_AUTO_GOAL_COMPLETED:
        return "completed";
    case HU_AUTO_GOAL_BLOCKED:
        return "blocked";
    case HU_AUTO_GOAL_ABANDONED:
        return "abandoned";
    default:
        return "unknown";
    }
}

void hu_goal_free(hu_allocator_t *alloc, hu_goal_t *goals, size_t count) {
    if (!alloc || !goals || count == 0)
        return;
    alloc->free(alloc->ctx, goals, count * sizeof(hu_goal_t));
}

#endif /* HU_ENABLE_SQLITE */
