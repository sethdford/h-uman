#include "human/memory/tiers.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

hu_error_t hu_tier_manager_create(hu_allocator_t *alloc,
#ifdef HU_ENABLE_SQLITE
                                  sqlite3 *db,
#else
                                  void *db,
#endif
                                  hu_tier_manager_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->alloc = alloc;
    out->db = db;
    out->core_token_budget = 500;
    out->recall_token_budget = 2000;
    return HU_OK;
}

void hu_tier_manager_deinit(hu_tier_manager_t *mgr) {
    if (mgr)
        memset(&mgr->core, 0, sizeof(mgr->core));
}

hu_error_t hu_tier_manager_init_tables(hu_tier_manager_t *mgr) {
    if (!mgr)
        return HU_ERR_INVALID_ARGUMENT;
#ifdef HU_ENABLE_SQLITE
    if (!mgr->db)
        return HU_OK;
    const char *sql = "CREATE TABLE IF NOT EXISTS tier_memory("
                      "key TEXT PRIMARY KEY, tier INTEGER, content TEXT, "
                      "created_at INTEGER, access_count INTEGER DEFAULT 0);"
                      "CREATE TABLE IF NOT EXISTS core_memory("
                      "field TEXT PRIMARY KEY, value TEXT, updated_at INTEGER);";
    char *err_msg = NULL;
    int rc = sqlite3_exec(mgr->db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
#endif
    return HU_OK;
}

hu_error_t hu_tier_manager_load_core(hu_tier_manager_t *mgr) {
    if (!mgr)
        return HU_ERR_INVALID_ARGUMENT;
#ifdef HU_ENABLE_SQLITE
    if (!mgr->db)
        return HU_OK;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(mgr->db, "SELECT field, value FROM core_memory", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *field = (const char *)sqlite3_column_text(stmt, 0);
        const char *value = (const char *)sqlite3_column_text(stmt, 1);
        if (!field || !value)
            continue;
        if (strcmp(field, "user_name") == 0) {
            strncpy(mgr->core.user_name, value, sizeof(mgr->core.user_name) - 1);
            mgr->core.user_name[sizeof(mgr->core.user_name) - 1] = '\0';
        } else if (strcmp(field, "user_bio") == 0) {
            strncpy(mgr->core.user_bio, value, sizeof(mgr->core.user_bio) - 1);
            mgr->core.user_bio[sizeof(mgr->core.user_bio) - 1] = '\0';
        } else if (strcmp(field, "user_preferences") == 0) {
            strncpy(mgr->core.user_preferences, value, sizeof(mgr->core.user_preferences) - 1);
            mgr->core.user_preferences[sizeof(mgr->core.user_preferences) - 1] = '\0';
        } else if (strcmp(field, "relationship_summary") == 0) {
            strncpy(mgr->core.relationship_summary, value,
                    sizeof(mgr->core.relationship_summary) - 1);
            mgr->core.relationship_summary[sizeof(mgr->core.relationship_summary) - 1] = '\0';
        } else if (strcmp(field, "active_goals") == 0) {
            strncpy(mgr->core.active_goals, value, sizeof(mgr->core.active_goals) - 1);
            mgr->core.active_goals[sizeof(mgr->core.active_goals) - 1] = '\0';
        }
    }
    sqlite3_finalize(stmt);
#endif
    return HU_OK;
}

hu_error_t hu_tier_manager_update_core(hu_tier_manager_t *mgr, const char *field, size_t field_len,
                                       const char *value, size_t value_len) {
    if (!mgr || !field || field_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!value && value_len != 0)
        return HU_ERR_INVALID_ARGUMENT;

    struct {
        const char *name;
        char *buf;
        size_t cap;
    } fields[] = {
        {"user_name", mgr->core.user_name, sizeof(mgr->core.user_name)},
        {"user_bio", mgr->core.user_bio, sizeof(mgr->core.user_bio)},
        {"user_preferences", mgr->core.user_preferences, sizeof(mgr->core.user_preferences)},
        {"relationship_summary", mgr->core.relationship_summary,
         sizeof(mgr->core.relationship_summary)},
        {"active_goals", mgr->core.active_goals, sizeof(mgr->core.active_goals)},
    };

    char *target = NULL;
    size_t target_cap = 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (strlen(fields[i].name) == field_len && strncmp(fields[i].name, field, field_len) == 0) {
            target = fields[i].buf;
            target_cap = fields[i].cap;
            break;
        }
    }
    if (!target)
        return HU_ERR_INVALID_ARGUMENT;

    /* Largest core field is user_preferences[1024] — stack snapshot for rollback */
    char saved[1024];
    if (target_cap > sizeof(saved))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(saved, target, target_cap);
    int64_t old_updated_at = mgr->core.updated_at;
    (void)old_updated_at;

    size_t copy_len = value_len < target_cap - 1 ? value_len : target_cap - 1;
    if (value && copy_len > 0)
        memcpy(target, value, copy_len);
    target[copy_len] = '\0';
    mgr->core.updated_at = (int64_t)time(NULL);

#ifdef HU_ENABLE_SQLITE
    if (mgr->db) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(
            mgr->db, "INSERT OR REPLACE INTO core_memory(field, value, updated_at) VALUES(?, ?, ?)",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            memcpy(target, saved, target_cap);
            mgr->core.updated_at = old_updated_at;
            return HU_ERR_MEMORY_BACKEND;
        }
        sqlite3_bind_text(stmt, 1, field, (int)field_len, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, target, (int)copy_len, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, mgr->core.updated_at);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            memcpy(target, saved, target_cap);
            mgr->core.updated_at = old_updated_at;
            return HU_ERR_MEMORY_BACKEND;
        }
    }
#endif
    return HU_OK;
}

hu_error_t hu_tier_manager_store(hu_tier_manager_t *mgr, hu_memory_tier_t tier, const char *key,
                                 size_t key_len, const char *content, size_t content_len) {
    if (!mgr || !key || key_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#ifdef HU_ENABLE_SQLITE
    if (!mgr->db)
        return HU_ERR_NOT_SUPPORTED;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        mgr->db,
        "INSERT OR REPLACE INTO tier_memory(key, tier, content, created_at) VALUES(?, ?, ?, ?)", -1,
        &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, key, (int)key_len, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)tier);
    if (content && content_len > 0)
        sqlite3_bind_text(stmt, 3, content, (int)content_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int64(stmt, 4, (int64_t)time(NULL));
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
#else
    (void)tier;
    (void)content;
    (void)content_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_tier_manager_promote(hu_tier_manager_t *mgr, const char *key, size_t key_len,
                                   hu_memory_tier_t from, hu_memory_tier_t to) {
    if (!mgr || !key || key_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (from == to)
        return HU_OK;
    if ((int)to >= (int)from)
        return HU_ERR_INVALID_ARGUMENT;
#ifdef HU_ENABLE_SQLITE
    if (!mgr->db)
        return HU_ERR_NOT_SUPPORTED;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        mgr->db, "UPDATE tier_memory SET tier = ? WHERE key = ? AND tier = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int(stmt, 1, (int)to);
    sqlite3_bind_text(stmt, 2, key, (int)key_len, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, (int)from);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
#else
    (void)from;
    (void)to;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_tier_manager_demote(hu_tier_manager_t *mgr, const char *key, size_t key_len,
                                  hu_memory_tier_t from, hu_memory_tier_t to) {
    if (!mgr || !key || key_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (from == to)
        return HU_OK;
    if ((int)to <= (int)from)
        return HU_ERR_INVALID_ARGUMENT;
#ifdef HU_ENABLE_SQLITE
    if (!mgr->db)
        return HU_ERR_NOT_SUPPORTED;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        mgr->db, "UPDATE tier_memory SET tier = ? WHERE key = ? AND tier = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int(stmt, 1, (int)to);
    sqlite3_bind_text(stmt, 2, key, (int)key_len, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, (int)from);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
#else
    (void)from;
    (void)to;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_tier_manager_build_core_prompt(hu_tier_manager_t *mgr, char *out, size_t out_cap,
                                             size_t *out_len) {
    if (!mgr || !out || out_cap == 0 || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out_len = 0;
    out[0] = '\0';
    if (!mgr->core.user_name[0] && !mgr->core.user_bio[0] && !mgr->core.user_preferences[0] &&
        !mgr->core.relationship_summary[0] && !mgr->core.active_goals[0])
        return HU_OK; /* a bare header is noise in every prompt */

    size_t written = 0;
    int n = snprintf(out, out_cap, "[Core Memory]\n");
    if (n > 0 && (size_t)n < out_cap)
        written = (size_t)n;
    else if (n > 0)
        written = out_cap - 1;

    struct {
        const char *label;
        const char *value;
    } items[] = {
        {"Name", mgr->core.user_name},
        {"Bio", mgr->core.user_bio},
        {"Preferences", mgr->core.user_preferences},
        {"Relationship", mgr->core.relationship_summary},
        {"Goals", mgr->core.active_goals},
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (items[i].value[0] != '\0' && written + 1 < out_cap) {
            n = snprintf(out + written, out_cap - written, "%s: %s\n", items[i].label,
                         items[i].value);
            if (n > 0 && written + (size_t)n < out_cap)
                written += (size_t)n;
            else if (n > 0) {
                written = out_cap - 1;
                break;
            }
        }
    }

    *out_len = written;
    return HU_OK;
}

hu_error_t hu_tier_manager_auto_tier(hu_tier_manager_t *mgr, const char *key, size_t key_len,
                                     const char *content, size_t content_len,
                                     hu_memory_tier_t *assigned) {
    if (!mgr || !key || !content || !assigned)
        return HU_ERR_INVALID_ARGUMENT;

    hu_memory_tier_t tier = HU_TIER_RECALL;

    static const char *core_markers[] = {"name is", "I am", "call me", "prefer"};
    for (size_t i = 0; i < sizeof(core_markers) / sizeof(core_markers[0]); i++) {
        size_t mlen = strlen(core_markers[i]);
        if (content_len >= mlen) {
            for (size_t j = 0; j + mlen <= content_len; j++) {
                bool match = true;
                for (size_t k = 0; k < mlen; k++) {
                    char a = content[j + k];
                    char b = core_markers[i][k];
                    if (a >= 'A' && a <= 'Z')
                        a = (char)(a + 32);
                    if (b >= 'A' && b <= 'Z')
                        b = (char)(b + 32);
                    if (a != b) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    tier = HU_TIER_CORE;
                    goto done;
                }
            }
        }
    }

    if (content_len >= 500)
        tier = HU_TIER_ARCHIVAL;

done:
    *assigned = tier;
    return hu_tier_manager_store(mgr, tier, key, key_len, content, content_len);
}

const char *hu_memory_tier_str(hu_memory_tier_t tier) {
    switch (tier) {
    case HU_TIER_CORE:
        return "core";
    case HU_TIER_RECALL:
        return "recall";
    case HU_TIER_ARCHIVAL:
        return "archival";
    default:
        return "unknown";
    }
}
