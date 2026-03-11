typedef int hu_oauth_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/feeds/oauth.h"
#include "human/core/error.h"
#include <sqlite3.h>
#include <string.h>

hu_error_t hu_oauth_store_token(sqlite3 *db, const char *provider,
                               const char *access_token, const char *refresh_token,
                               int64_t expires_at, const char *scope) {
    if (!db || !provider || !access_token)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] =
        "INSERT OR REPLACE INTO oauth_tokens (provider, access_token, refresh_token, expires_at, scope) "
        "VALUES (?, ?, ?, ?, ?)";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, (int)(sizeof(sql) - 1), &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, access_token, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, refresh_token ? refresh_token : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, expires_at);
    sqlite3_bind_text(stmt, 5, scope ? scope : "", -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    return HU_OK;
}

hu_error_t hu_oauth_get_token(hu_allocator_t *alloc, sqlite3 *db,
                             const char *provider, char *access_token, size_t at_cap,
                             size_t *at_len) {
    (void)alloc;
    if (!db || !provider || !access_token || !at_len)
        return HU_ERR_INVALID_ARGUMENT;

    if (at_cap == 0) {
        *at_len = 0;
        return HU_OK;
    }

    static const char sql[] =
        "SELECT access_token FROM oauth_tokens WHERE provider = ?";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, (int)(sizeof(sql) - 1), &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_RECALL;

    sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        size_t len = val ? strlen(val) : 0;
        if (len >= at_cap)
            len = at_cap - 1;
        if (val && len > 0)
            memcpy(access_token, val, len);
        access_token[len] = '\0';
        *at_len = len;
    } else {
        access_token[0] = '\0';
        *at_len = 0;
    }

    sqlite3_finalize(stmt);
    return HU_OK;
}

bool hu_oauth_is_expired(sqlite3 *db, const char *provider, int64_t now_ts) {
    if (!db || !provider)
        return true;

    static const char sql[] =
        "SELECT expires_at FROM oauth_tokens WHERE provider = ?";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, (int)(sizeof(sql) - 1), &stmt, NULL);
    if (rc != SQLITE_OK)
        return true;

    sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);

    bool expired = true;
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) == SQLITE_NULL)
            expired = true;
        else {
            int64_t expires_at = sqlite3_column_int64(stmt, 0);
            expired = (expires_at - 300 < now_ts);
        }
    }

    sqlite3_finalize(stmt);
    return expired;
}

#endif /* HU_ENABLE_SQLITE */
