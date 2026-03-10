#include "human/tools/database.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#define TOOL_NAME "database"
#define TOOL_DESC "Execute SQL queries against a database. Read-only by default."
#define TOOL_PARAMS                                                                 \
    "{\"type\":\"object\",\"properties\":{"                                         \
    "\"action\":{\"type\":\"string\",\"enum\":[\"query\",\"execute\",\"tables\"]}," \
    "\"sql\":{\"type\":\"string\"},"                                                \
    "\"connection\":{\"type\":\"string\"}"                                          \
    "},\"required\":[\"action\"]}"

typedef struct {
    hu_allocator_t *alloc;
    char *default_db_path;
} database_ctx_t;

static hu_error_t database_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                   hu_tool_result_t *out) {
    database_ctx_t *c = (database_ctx_t *)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *action = hu_json_get_string(args, "action");
    if (!action) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }

#if HU_IS_TEST || !defined(HU_ENABLE_SQLITE)
    (void)c;
    const char *sql = hu_json_get_string(args, "sql");
    if (strcmp(action, "tables") == 0) {
        *out = hu_tool_result_ok("{\"tables\":[]}", 13);
    } else if (strcmp(action, "query") == 0 || strcmp(action, "execute") == 0) {
        char *msg = hu_sprintf(alloc, "SQL executed: %s", sql ? sql : "(none)");
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else {
        *out = hu_tool_result_fail("unknown action", 14);
    }
    return HU_OK;
#else
    const char *sql = hu_json_get_string(args, "sql");
    const char *db_path = c->default_db_path ? c->default_db_path : ":memory:";

    sqlite3 *db = NULL;
    int flags = (strcmp(action, "execute") == 0) ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY;
    if (sqlite3_open_v2(db_path, &db, flags, NULL) != SQLITE_OK) {
        *out = hu_tool_result_fail("cannot open database", 20);
        return HU_OK;
    }

    if (strcmp(action, "tables") == 0) {
        const char *tq = "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, tq, -1, &stmt, NULL) == SQLITE_OK) {
            size_t cap = 256;
            char *buf = (char *)alloc->alloc(alloc->ctx, cap);
            size_t off = 0;
            if (buf) {
                off += (size_t)snprintf(buf + off, cap - off, "{\"tables\":[");
                int first = 1;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *nm = (const char *)sqlite3_column_text(stmt, 0);
                    if (!first && off < cap)
                        buf[off++] = ',';
                    off += (size_t)snprintf(buf + off, cap - off, "\"%s\"", nm ? nm : "");
                    first = 0;
                }
                off += (size_t)snprintf(buf + off, cap - off, "]}");
                *out = hu_tool_result_ok_owned(buf, off);
            } else {
                *out = hu_tool_result_fail("out of memory", 12);
            }
            sqlite3_finalize(stmt);
        } else {
            *out = hu_tool_result_fail("query failed", 12);
        }
    } else if (sql) {
        /* Reject dangerous SQL statements — only allow SELECT for query, and
           SELECT/INSERT/UPDATE/DELETE for execute.  Block ATTACH, PRAGMA, LOAD,
           and other meta-commands that could escape the database boundary. */
        const char *s = sql;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        int is_select = (strncasecmp(s, "SELECT", 6) == 0 && (s[6] == ' ' || s[6] == '\t'));
        int is_write = (strncasecmp(s, "INSERT", 6) == 0 || strncasecmp(s, "UPDATE", 6) == 0 ||
                        strncasecmp(s, "DELETE", 6) == 0) &&
                       (s[6] == ' ' || s[6] == '\t');
        if (strcmp(action, "query") == 0 && !is_select) {
            sqlite3_close(db);
            *out = hu_tool_result_fail("query action only allows SELECT", 31);
            return HU_OK;
        }
        if (strcmp(action, "execute") == 0 && !is_select && !is_write) {
            sqlite3_close(db);
            *out = hu_tool_result_fail("execute only allows SELECT/INSERT/UPDATE/DELETE", 47);
            return HU_OK;
        }

        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            int rows = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW)
                rows++;
            sqlite3_finalize(stmt);
            char *msg = hu_sprintf(alloc, "{\"rows_affected\":%d}", rows);
            *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
        } else {
            const char *errmsg = sqlite3_errmsg(db);
            *out = hu_tool_result_fail(errmsg, errmsg ? strlen(errmsg) : 0);
        }
    } else {
        *out = hu_tool_result_fail("missing sql", 11);
    }

    sqlite3_close(db);
    return HU_OK;
#endif
}

static const char *database_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *database_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *database_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void database_deinit(void *ctx, hu_allocator_t *alloc) {
    database_ctx_t *c = (database_ctx_t *)ctx;
    if (!c)
        return;
    if (c->default_db_path)
        alloc->free(alloc->ctx, c->default_db_path, strlen(c->default_db_path) + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_tool_vtable_t database_vtable = {
    .execute = database_execute,
    .name = database_name,
    .description = database_desc,
    .parameters_json = database_params,
    .deinit = database_deinit,
};

hu_error_t hu_database_tool_create(hu_allocator_t *alloc, const char *default_db_path,
                                   hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    database_ctx_t *c = (database_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->default_db_path =
        default_db_path ? hu_strndup(alloc, default_db_path, strlen(default_db_path)) : NULL;
    out->ctx = c;
    out->vtable = &database_vtable;
    return HU_OK;
}
