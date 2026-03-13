/*
 * Self-Improvement Engine — closes the reflection → behavior feedback loop.
 * Reads recent reflections, derives prompt patches and tool preferences.
 */

#ifdef HU_ENABLE_SQLITE

#include "human/intelligence/self_improve.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/slice.h"
#include "human/core/string.h"
#include <sqlite3.h>
#include <stddef.h>
#include <string.h>

#define PATCH_TEXT_MAX 2048
#define TOOL_NAME_MAX  128

hu_error_t hu_self_improve_create(hu_allocator_t *alloc, sqlite3 *db,
                                  hu_self_improve_t *out) {
    if (!alloc || !db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->alloc = alloc;
    out->db = db;
    return HU_OK;
}

void hu_self_improve_deinit(hu_self_improve_t *engine) {
    (void)engine;
    /* Caller owns db; no-op */
}

hu_error_t hu_self_improve_init_tables(hu_self_improve_t *engine) {
    if (!engine || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    const char *sql_patches =
        "CREATE TABLE IF NOT EXISTS prompt_patches("
        "id INTEGER PRIMARY KEY, source TEXT, patch_text TEXT, "
        "active INTEGER DEFAULT 1, applied_at INTEGER)";
    int rc = sqlite3_exec(engine->db, sql_patches, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    const char *sql_prefs =
        "CREATE TABLE IF NOT EXISTS tool_prefs("
        "tool_name TEXT PRIMARY KEY, weight REAL DEFAULT 1.0, "
        "successes INTEGER DEFAULT 0, failures INTEGER DEFAULT 0, updated_at INTEGER)";
    rc = sqlite3_exec(engine->db, sql_prefs, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    return HU_OK;
}

/* Extract patch text from recommendations (simple string, no LLM). */
static void extract_patch_text(const char *rec, size_t rec_len, char *out, size_t out_cap) {
    if (!rec || rec_len == 0 || !out || out_cap == 0) {
        if (out && out_cap > 0)
            out[0] = '\0';
        return;
    }
    size_t copy_len = rec_len < out_cap - 1 ? rec_len : out_cap - 1;
    memcpy(out, rec, copy_len);
    out[copy_len] = '\0';
    /* Trim trailing whitespace */
    while (copy_len > 0 && (out[copy_len - 1] == ' ' || out[copy_len - 1] == '\t' ||
                            out[copy_len - 1] == '\n' || out[copy_len - 1] == '\r')) {
        out[--copy_len] = '\0';
    }
}

hu_error_t hu_self_improve_apply_reflections(hu_self_improve_t *engine, int64_t now_ts) {
    (void)now_ts;
    if (!engine || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    /* Get max applied_at from prompt_patches to find reflections since last patch */
    const char *max_sql = "SELECT COALESCE(MAX(applied_at), 0) FROM prompt_patches";
    sqlite3_stmt *max_stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, max_sql, -1, &max_stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    int64_t since = 0;
    if (sqlite3_step(max_stmt) == SQLITE_ROW)
        since = sqlite3_column_int64(max_stmt, 0);
    sqlite3_finalize(max_stmt);

    /* Query self_evaluations since last patch; use recommendations as patch source */
    const char *sel =
        "SELECT id, recommendations, created_at FROM self_evaluations "
        "WHERE created_at > ?1 AND recommendations IS NOT NULL AND recommendations != '' "
        "ORDER BY created_at ASC";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(engine->db, sel, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int64(stmt, 1, since);

    const char *ins =
        "INSERT INTO prompt_patches (source, patch_text, active, applied_at) "
        "VALUES (?1, ?2, 1, ?3)";
    sqlite3_stmt *ins_stmt = NULL;
    rc = sqlite3_prepare_v2(engine->db, ins, -1, &ins_stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return HU_ERR_MEMORY_STORE;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *rec = (const char *)sqlite3_column_text(stmt, 1);
        int64_t created_at = sqlite3_column_int64(stmt, 2);
        if (!rec)
            continue;
        size_t rec_len = (size_t)sqlite3_column_bytes(stmt, 1);
        char patch_buf[PATCH_TEXT_MAX];
        extract_patch_text(rec, rec_len, patch_buf, sizeof(patch_buf));
        if (patch_buf[0] == '\0')
            continue;

        sqlite3_bind_text(ins_stmt, 1, "reflection", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins_stmt, 2, patch_buf, -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins_stmt, 3, created_at);
        rc = sqlite3_step(ins_stmt);
        sqlite3_reset(ins_stmt);
        sqlite3_clear_bindings(ins_stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(ins_stmt);
            sqlite3_finalize(stmt);
            return HU_ERR_MEMORY_STORE;
        }
    }

    sqlite3_finalize(ins_stmt);
    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_error_t hu_self_improve_record_tool_outcome(hu_self_improve_t *engine,
                                               const char *tool_name, size_t name_len,
                                               bool succeeded, int64_t now_ts) {
    if (!engine || !engine->db || !tool_name)
        return HU_ERR_INVALID_ARGUMENT;
    if (name_len == 0)
        name_len = strlen(tool_name);
    if (name_len > TOOL_NAME_MAX - 1)
        name_len = TOOL_NAME_MAX - 1;

    /* Fetch current row if exists */
    const char *sel = "SELECT successes, failures FROM tool_prefs WHERE tool_name = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sel, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    char name_buf[TOOL_NAME_MAX];
    memcpy(name_buf, tool_name, name_len);
    name_buf[name_len] = '\0';
    sqlite3_bind_text(stmt, 1, name_buf, -1, SQLITE_STATIC);

    int32_t successes = 0;
    int32_t failures = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        successes = (int32_t)sqlite3_column_int(stmt, 0);
        failures = (int32_t)sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);

    if (succeeded)
        successes++;
    else
        failures++;

    int total = successes + failures;
    double weight = (total > 0) ? (double)successes / (double)total : 1.0;
    if (weight < 0.1)
        weight = 0.1;
    if (weight > 2.0)
        weight = 2.0;

    const char *upsert =
        "INSERT OR REPLACE INTO tool_prefs (tool_name, weight, successes, failures, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5)";
    rc = sqlite3_prepare_v2(engine->db, upsert, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, name_buf, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, weight);
    sqlite3_bind_int(stmt, 3, successes);
    sqlite3_bind_int(stmt, 4, failures);
    sqlite3_bind_int64(stmt, 5, now_ts);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_STORE;
}

double hu_self_improve_get_tool_weight(hu_self_improve_t *engine,
                                       const char *tool_name, size_t name_len) {
    if (!engine || !engine->db || !tool_name)
        return 1.0;
    if (name_len == 0)
        name_len = strlen(tool_name);
    if (name_len > TOOL_NAME_MAX - 1)
        name_len = TOOL_NAME_MAX - 1;

    char name_buf[TOOL_NAME_MAX];
    memcpy(name_buf, tool_name, name_len);
    name_buf[name_len] = '\0';

    const char *sql = "SELECT weight FROM tool_prefs WHERE tool_name = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return 1.0;
    sqlite3_bind_text(stmt, 1, name_buf, -1, SQLITE_STATIC);
    double weight = 1.0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        weight = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return weight;
}

hu_error_t hu_self_improve_get_prompt_patches(hu_self_improve_t *engine,
                                              char **out, size_t *out_len) {
    if (!engine || !engine->db || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *sql =
        "SELECT patch_text FROM prompt_patches WHERE active = 1 "
        "ORDER BY applied_at DESC LIMIT 10";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    char *copies[10];
    hu_str_t parts[10];
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < 10) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        int bytes = sqlite3_column_bytes(stmt, 0);
        if (text && text[0] && bytes > 0) {
            size_t len = (size_t)bytes;
            char *copy = (char *)engine->alloc->alloc(engine->alloc->ctx, len + 1);
            if (!copy) {
                for (size_t j = 0; j < count; j++)
                    engine->alloc->free(engine->alloc->ctx, copies[j], parts[j].len + 1);
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(copy, text, len);
            copy[len] = '\0';
            copies[count] = copy;
            parts[count].ptr = copy;
            parts[count].len = len;
            count++;
        }
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        char *empty = (char *)engine->alloc->alloc(engine->alloc->ctx, 1);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        empty[0] = '\0';
        *out = empty;
        *out_len = 0;
        return HU_OK;
    }

    hu_str_t sep = HU_STR_LIT("\n");
    char *joined = hu_str_join(engine->alloc, parts, count, sep);
    for (size_t i = 0; i < count; i++)
        engine->alloc->free(engine->alloc->ctx, copies[i], parts[i].len + 1);
    if (!joined)
        return HU_ERR_OUT_OF_MEMORY;
    *out = joined;
    *out_len = strlen(joined);
    return HU_OK;
}

hu_error_t hu_self_improve_active_patch_count(hu_self_improve_t *engine, size_t *out) {
    if (!engine || !engine->db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = 0;

    const char *sql = "SELECT COUNT(*) FROM prompt_patches WHERE active = 1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
