#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/intelligence/skills.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static hu_error_t load_skill_by_id(sqlite3 *db, int64_t skill_id, hu_skill_t *out);
static hu_error_t resolve_chain_recursive(hu_allocator_t *alloc, sqlite3 *db,
    const char *strategy, size_t strategy_len,
    char *out, size_t out_cap, size_t *out_len, int depth);

hu_error_t hu_skill_insert(hu_allocator_t *alloc, sqlite3 *db, const char *name, size_t name_len,
                          const char *type, size_t type_len,
                          const char *contact_id, size_t cid_len,
                          const char *trigger_conditions, size_t tc_len,
                          const char *strategy, size_t strat_len,
                          const char *origin, size_t origin_len,
                          int64_t parent_skill_id, int64_t now_ts,
                          int64_t *out_id) {
    (void)alloc;
    if (!db || !out_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (!name || name_len == 0 || !type || type_len == 0 || !strategy || strat_len == 0 ||
        !origin || origin_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO skills (name, type, contact_id, trigger_conditions, "
                                "strategy, success_rate, attempts, successes, version, origin, "
                                "parent_skill_id, created_at, updated_at, retired) "
                                "VALUES (?, ?, ?, ?, ?, 0.5, 0, 0, 1, ?, ?, ?, ?, 0)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, name, (int)name_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, type, (int)type_len, SQLITE_STATIC);
    if (contact_id && cid_len > 0)
        sqlite3_bind_text(stmt, 3, contact_id, (int)cid_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    if (trigger_conditions && tc_len > 0)
        sqlite3_bind_text(stmt, 4, trigger_conditions, (int)tc_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_text(stmt, 5, strategy, (int)strat_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, origin, (int)origin_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 7, parent_skill_id);
    sqlite3_bind_int64(stmt, 8, now_ts);
    sqlite3_bind_int64(stmt, 9, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;

    *out_id = sqlite3_last_insert_rowid(db);
    return HU_OK;
}

hu_error_t hu_skill_load_active(hu_allocator_t *alloc, sqlite3 *db, const char *contact_id,
                                size_t cid_len, hu_skill_t **out, size_t *out_count) {
    if (!alloc || !db || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    sqlite3_stmt *stmt = NULL;
    int rc;
    if (contact_id && cid_len > 0) {
        rc = sqlite3_prepare_v2(db,
                                "SELECT id, name, type, contact_id, trigger_conditions, strategy, "
                                "success_rate, attempts, successes, version, origin, "
                                "parent_skill_id, created_at, updated_at, retired "
                                "FROM skills WHERE retired=0 AND (contact_id=? OR contact_id IS NULL) "
                                "ORDER BY success_rate DESC",
                                -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return HU_ERR_MEMORY_BACKEND;
        sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);
    } else {
        rc = sqlite3_prepare_v2(db,
                                "SELECT id, name, type, contact_id, trigger_conditions, strategy, "
                                "success_rate, attempts, successes, version, origin, "
                                "parent_skill_id, created_at, updated_at, retired "
                                "FROM skills WHERE retired=0 AND contact_id IS NULL "
                                "ORDER BY success_rate DESC",
                                -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return HU_ERR_MEMORY_BACKEND;
    }

    size_t cap = 0;
    size_t n = 0;
    hu_skill_t *arr = NULL;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            hu_skill_t *new_arr = (hu_skill_t *)alloc->realloc(alloc->ctx, arr,
                                                               cap * sizeof(hu_skill_t),
                                                               new_cap * sizeof(hu_skill_t));
            if (!new_arr) {
                sqlite3_finalize(stmt);
                hu_skill_free(alloc, arr, n);
                return HU_ERR_OUT_OF_MEMORY;
            }
            arr = new_arr;
            cap = new_cap;
        }

        hu_skill_t *s = &arr[n];
        memset(s, 0, sizeof(*s));
        s->id = sqlite3_column_int64(stmt, 0);

        const char *c = (const char *)sqlite3_column_text(stmt, 1);
        if (c) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 1);
            if (len > sizeof(s->name) - 1)
                len = sizeof(s->name) - 1;
            memcpy(s->name, c, len);
            s->name[len] = '\0';
        }
        c = (const char *)sqlite3_column_text(stmt, 2);
        if (c) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 2);
            if (len > sizeof(s->type) - 1)
                len = sizeof(s->type) - 1;
            memcpy(s->type, c, len);
            s->type[len] = '\0';
        }
        c = (const char *)sqlite3_column_text(stmt, 3);
        if (c) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 3);
            if (len > sizeof(s->contact_id) - 1)
                len = sizeof(s->contact_id) - 1;
            memcpy(s->contact_id, c, len);
            s->contact_id[len] = '\0';
        }
        c = (const char *)sqlite3_column_text(stmt, 4);
        if (c) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 4);
            if (len > sizeof(s->trigger_conditions) - 1)
                len = sizeof(s->trigger_conditions) - 1;
            memcpy(s->trigger_conditions, c, len);
            s->trigger_conditions[len] = '\0';
        }
        c = (const char *)sqlite3_column_text(stmt, 5);
        if (c) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 5);
            s->strategy_len = len;
            if (len > sizeof(s->strategy) - 1)
                len = sizeof(s->strategy) - 1;
            memcpy(s->strategy, c, len);
            s->strategy[len] = '\0';
        }
        s->success_rate = sqlite3_column_double(stmt, 6);
        s->attempts = sqlite3_column_int(stmt, 7);
        s->successes = sqlite3_column_int(stmt, 8);
        s->version = sqlite3_column_int(stmt, 9);
        c = (const char *)sqlite3_column_text(stmt, 10);
        if (c) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 10);
            if (len > sizeof(s->origin) - 1)
                len = sizeof(s->origin) - 1;
            memcpy(s->origin, c, len);
            s->origin[len] = '\0';
        }
        s->parent_skill_id = sqlite3_column_int64(stmt, 11);
        s->created_at = sqlite3_column_int64(stmt, 12);
        s->updated_at = sqlite3_column_int64(stmt, 13);
        s->retired = sqlite3_column_int(stmt, 14);

        n++;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        return HU_ERR_MEMORY_BACKEND;

    *out = arr;
    *out_count = n;
    return HU_OK;
}

hu_error_t hu_skill_get_by_name(hu_allocator_t *alloc, sqlite3 *db, const char *name,
                                size_t name_len, hu_skill_t *out) {
    (void)alloc;
    if (!db || !name || name_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT id, name, type, contact_id, trigger_conditions, strategy, "
                                "success_rate, attempts, successes, version, origin, "
                                "parent_skill_id, created_at, updated_at, retired "
                                "FROM skills WHERE name=? AND retired=0",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, name, (int)name_len, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return HU_ERR_NOT_FOUND;
    }

    out->id = sqlite3_column_int64(stmt, 0);
    const char *c = (const char *)sqlite3_column_text(stmt, 1);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 1);
        if (len > sizeof(out->name) - 1)
            len = sizeof(out->name) - 1;
        memcpy(out->name, c, len);
        out->name[len] = '\0';
    }
    c = (const char *)sqlite3_column_text(stmt, 2);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 2);
        if (len > sizeof(out->type) - 1)
            len = sizeof(out->type) - 1;
        memcpy(out->type, c, len);
        out->type[len] = '\0';
    }
    c = (const char *)sqlite3_column_text(stmt, 3);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 3);
        if (len > sizeof(out->contact_id) - 1)
            len = sizeof(out->contact_id) - 1;
        memcpy(out->contact_id, c, len);
        out->contact_id[len] = '\0';
    }
    c = (const char *)sqlite3_column_text(stmt, 4);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 4);
        if (len > sizeof(out->trigger_conditions) - 1)
            len = sizeof(out->trigger_conditions) - 1;
        memcpy(out->trigger_conditions, c, len);
        out->trigger_conditions[len] = '\0';
    }
    c = (const char *)sqlite3_column_text(stmt, 5);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 5);
        out->strategy_len = len;
        if (len > sizeof(out->strategy) - 1)
            len = sizeof(out->strategy) - 1;
        memcpy(out->strategy, c, len);
        out->strategy[len] = '\0';
    }
    out->success_rate = sqlite3_column_double(stmt, 6);
    out->attempts = sqlite3_column_int(stmt, 7);
    out->successes = sqlite3_column_int(stmt, 8);
    out->version = sqlite3_column_int(stmt, 9);
    c = (const char *)sqlite3_column_text(stmt, 10);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 10);
        if (len > sizeof(out->origin) - 1)
            len = sizeof(out->origin) - 1;
        memcpy(out->origin, c, len);
        out->origin[len] = '\0';
    }
    out->parent_skill_id = sqlite3_column_int64(stmt, 11);
    out->created_at = sqlite3_column_int64(stmt, 12);
    out->updated_at = sqlite3_column_int64(stmt, 13);
    out->retired = sqlite3_column_int(stmt, 14);

    sqlite3_finalize(stmt);
    return HU_OK;
}

static hu_error_t load_skill_by_id(sqlite3 *db, int64_t skill_id, hu_skill_t *out) {
    if (!db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT id, name, type, contact_id, trigger_conditions, strategy, "
                                "success_rate, attempts, successes, version, origin, "
                                "parent_skill_id, created_at, updated_at, retired "
                                "FROM skills WHERE id=?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_int64(stmt, 1, skill_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return HU_ERR_NOT_FOUND;
    }
    out->id = sqlite3_column_int64(stmt, 0);
    const char *c = (const char *)sqlite3_column_text(stmt, 1);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 1);
        if (len > sizeof(out->name) - 1)
            len = sizeof(out->name) - 1;
        memcpy(out->name, c, len);
        out->name[len] = '\0';
    }
    c = (const char *)sqlite3_column_text(stmt, 2);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 2);
        if (len > sizeof(out->type) - 1)
            len = sizeof(out->type) - 1;
        memcpy(out->type, c, len);
        out->type[len] = '\0';
    }
    c = (const char *)sqlite3_column_text(stmt, 3);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 3);
        if (len > sizeof(out->contact_id) - 1)
            len = sizeof(out->contact_id) - 1;
        memcpy(out->contact_id, c, len);
        out->contact_id[len] = '\0';
    }
    c = (const char *)sqlite3_column_text(stmt, 4);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 4);
        if (len > sizeof(out->trigger_conditions) - 1)
            len = sizeof(out->trigger_conditions) - 1;
        memcpy(out->trigger_conditions, c, len);
        out->trigger_conditions[len] = '\0';
    }
    c = (const char *)sqlite3_column_text(stmt, 5);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 5);
        out->strategy_len = len;
        if (len > sizeof(out->strategy) - 1)
            len = sizeof(out->strategy) - 1;
        memcpy(out->strategy, c, len);
        out->strategy[len] = '\0';
    }
    out->success_rate = sqlite3_column_double(stmt, 6);
    out->attempts = sqlite3_column_int(stmt, 7);
    out->successes = sqlite3_column_int(stmt, 8);
    out->version = sqlite3_column_int(stmt, 9);
    c = (const char *)sqlite3_column_text(stmt, 10);
    if (c) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 10);
        if (len > sizeof(out->origin) - 1)
            len = sizeof(out->origin) - 1;
        memcpy(out->origin, c, len);
        out->origin[len] = '\0';
    }
    out->parent_skill_id = sqlite3_column_int64(stmt, 11);
    out->created_at = sqlite3_column_int64(stmt, 12);
    out->updated_at = sqlite3_column_int64(stmt, 13);
    out->retired = sqlite3_column_int(stmt, 14);
    sqlite3_finalize(stmt);
    return HU_OK;
}

void hu_skill_free(hu_allocator_t *alloc, hu_skill_t *skills, size_t count) {
    if (!alloc || !skills || count == 0)
        return;
    alloc->free(alloc->ctx, skills, count * sizeof(hu_skill_t));
}

/* Parse trigger_conditions "emotion==X,topic==Y,confidence>=Z,contact==W" and check match. */
static int trigger_conditions_match(const char *tc, size_t tc_len,
    const char *contact_id, size_t cid_len,
    const char *emotion, size_t emotion_len,
    const char *topic, size_t topic_len,
    double confidence) {
    if (!tc || tc_len == 0)
        return 1; /* no conditions = always match */
    /* Copy to parse (conditions may contain embedded commas in future; keep simple for now) */
    char buf[512];
    if (tc_len >= sizeof(buf))
        return 0;
    memcpy(buf, tc, tc_len);
    buf[tc_len] = '\0';
    const char *p = buf;
    while (*p) {
        const char *start = p;
        while (*p && *p != ',')
            p++;
        /* start..p is one condition: key==value or key>=value */
        const char *eq = strchr(start, '=');
        if (!eq || eq >= p)
            return 0;
        size_t key_len = (size_t)(eq - start);
        if (key_len > 0 && *(eq - 1) == '>')
            key_len--;
        const char *val = eq + 1;
        if (*val == '=')
            val++;
        size_t val_len = (size_t)(p - val);
        if (val_len == 0)
            return 0;
        if (key_len == 7 && memcmp(start, "emotion", 7) == 0) {
            if (!emotion || emotion_len != val_len || memcmp(emotion, val, val_len) != 0)
                return 0;
        } else if (key_len == 5 && memcmp(start, "topic", 5) == 0) {
            if (!topic || topic_len != val_len || memcmp(topic, val, val_len) != 0)
                return 0;
        } else if (key_len == 7 && memcmp(start, "contact", 7) == 0) {
            if (!contact_id || cid_len != val_len || memcmp(contact_id, val, val_len) != 0)
                return 0;
        } else if (key_len == 10 && memcmp(start, "confidence", 10) == 0) {
            double thresh = strtod(val, NULL);
            if (confidence < thresh)
                return 0;
        }
        if (*p == ',')
            p++;
    }
    return 1;
}

hu_error_t hu_skill_match_triggers(hu_allocator_t *alloc, sqlite3 *db,
    const char *contact_id, size_t cid_len,
    const char *emotion, size_t emotion_len,
    const char *topic, size_t topic_len,
    double confidence,
    hu_skill_t **out, size_t *out_count) {
    if (!alloc || !db || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_skill_t *active = NULL;
    size_t active_count = 0;
    hu_error_t err = hu_skill_load_active(alloc, db, contact_id, cid_len, &active, &active_count);
    if (err != HU_OK)
        return err;
    if (!active || active_count == 0) {
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }

    size_t cap = 0;
    size_t n = 0;
    hu_skill_t *arr = NULL;
    for (size_t i = 0; i < active_count; i++) {
        const hu_skill_t *s = &active[i];
        if (!trigger_conditions_match(s->trigger_conditions, strlen(s->trigger_conditions),
                contact_id, cid_len, emotion, emotion_len, topic, topic_len, confidence))
            continue;
        if (n >= cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            hu_skill_t *new_arr = (hu_skill_t *)alloc->realloc(alloc->ctx, arr,
                                                               cap * sizeof(hu_skill_t),
                                                               new_cap * sizeof(hu_skill_t));
            if (!new_arr) {
                hu_skill_free(alloc, active, active_count);
                hu_skill_free(alloc, arr, n);
                return HU_ERR_OUT_OF_MEMORY;
            }
            arr = new_arr;
            cap = new_cap;
        }
        memcpy(&arr[n], s, sizeof(hu_skill_t));
        n++;
    }
    hu_skill_free(alloc, active, active_count);
    *out = arr;
    *out_count = n;
    return HU_OK;
}

hu_error_t hu_skill_record_attempt(sqlite3 *db,
    int64_t skill_id, const char *contact_id, size_t cid_len,
    int64_t applied_at,
    const char *outcome_signal, size_t sig_len,
    const char *outcome_evidence, size_t ev_len,
    const char *context, size_t ctx_len,
    int64_t *out_id) {
    if (!db || !out_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (!contact_id || cid_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO skill_attempts (skill_id, contact_id, applied_at, outcome_signal, "
        "outcome_evidence, context) VALUES (?, ?, ?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, skill_id);
    sqlite3_bind_text(stmt, 2, contact_id, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, applied_at);
    if (outcome_signal && sig_len > 0)
        sqlite3_bind_text(stmt, 4, outcome_signal, (int)sig_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    if (outcome_evidence && ev_len > 0)
        sqlite3_bind_text(stmt, 5, outcome_evidence, (int)ev_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);
    if (context && ctx_len > 0)
        sqlite3_bind_text(stmt, 6, context, (int)ctx_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;

    *out_id = sqlite3_last_insert_rowid(db);
    return HU_OK;
}

hu_error_t hu_skill_update_success_rate(sqlite3 *db,
    int64_t skill_id, int new_attempts, int new_successes) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (new_attempts < 0 || new_successes < 0 || new_successes > new_attempts)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t now = (int64_t)time(NULL);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE skills SET attempts=?, successes=?, "
        "success_rate=CASE WHEN ? > 0 THEN CAST(? AS REAL)/CAST(? AS REAL) ELSE 0.5 END, "
        "updated_at=? WHERE id=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int(stmt, 1, new_attempts);
    sqlite3_bind_int(stmt, 2, new_successes);
    sqlite3_bind_int(stmt, 3, new_attempts);
    sqlite3_bind_int(stmt, 4, new_successes);
    sqlite3_bind_int(stmt, 5, new_attempts);
    sqlite3_bind_int64(stmt, 6, now);
    sqlite3_bind_int64(stmt, 7, skill_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    return HU_OK;
}

hu_error_t hu_skill_evolve(hu_allocator_t *alloc, sqlite3 *db,
    int64_t skill_id,
    const char *new_strategy, size_t strat_len,
    const char *reason, size_t reason_len,
    int64_t now_ts) {
    (void)alloc;
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (!new_strategy || strat_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_skill_t old = {0};
    hu_error_t err = load_skill_by_id(db, skill_id, &old);
    if (err != HU_OK)
        return err;

    char old_strategy[2048];
    size_t old_len = old.strategy_len;
    if (old_len > sizeof(old_strategy) - 1)
        old_len = sizeof(old_strategy) - 1;
    memcpy(old_strategy, old.strategy, old_len);
    old_strategy[old_len] = '\0';

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO skill_evolution (skill_id, version, strategy, success_rate, evolved_at, reason) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, skill_id);
    sqlite3_bind_int(stmt, 2, old.version);
    sqlite3_bind_text(stmt, 3, old_strategy, (int)old_len, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, old.success_rate);
    sqlite3_bind_int64(stmt, 5, now_ts);
    if (reason && reason_len > 0)
        sqlite3_bind_text(stmt, 6, reason, (int)reason_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;

    rc = sqlite3_prepare_v2(db,
        "UPDATE skills SET strategy=?, version=version+1, updated_at=?, "
        "success_rate=0.5, attempts=0, successes=0 WHERE id=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, new_strategy, (int)strat_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now_ts);
    sqlite3_bind_int64(stmt, 3, skill_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    return HU_OK;
}

hu_error_t hu_skill_retire(sqlite3 *db, int64_t skill_id) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE skills SET retired=1 WHERE id=?", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(stmt, 1, skill_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    return HU_OK;
}

bool hu_skill_db_should_retire(int version, double success_rate) {
    return version >= 3 && success_rate < 0.35;
}

hu_error_t hu_skill_transfer(hu_allocator_t *alloc, sqlite3 *db,
    int64_t skill_id,
    const char *new_trigger, size_t trigger_len,
    double confidence_penalty,
    int64_t now_ts,
    int64_t *out_id) {
    (void)confidence_penalty;
    if (!alloc || !db || !out_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (!new_trigger || trigger_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_skill_t orig = {0};
    hu_error_t err = load_skill_by_id(db, skill_id, &orig);
    if (err != HU_OK)
        return err;

    char new_strategy[2048];
    static const char prefix[] = "Generalized: ";
    size_t prefix_len = sizeof(prefix) - 1;
    size_t orig_len = orig.strategy_len;
    if (orig_len > sizeof(orig.strategy) - 1)
        orig_len = sizeof(orig.strategy) - 1;
    if (prefix_len + orig_len >= sizeof(new_strategy))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(new_strategy, prefix, prefix_len);
    memcpy(new_strategy + prefix_len, orig.strategy, orig_len);
    new_strategy[prefix_len + orig_len] = '\0';
    size_t new_strat_len = prefix_len + orig_len;

    return hu_skill_insert(alloc, db,
        orig.name, strlen(orig.name),
        orig.type, strlen(orig.type),
        NULL, 0,
        new_trigger, trigger_len,
        new_strategy, new_strat_len,
        "transfer", 8,
        skill_id, now_ts, out_id);
}

#define SKILL_PREFIX "skill:"
#define SKILL_PREFIX_LEN 6

hu_error_t hu_skill_resolve_chain(hu_allocator_t *alloc, sqlite3 *db,
    const char *strategy, size_t strategy_len,
    char *out, size_t out_cap, size_t *out_len) {
    if (!alloc || !db || !out || out_cap == 0 || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!strategy || strategy_len == 0) {
        out[0] = '\0';
        *out_len = 0;
        return HU_OK;
    }

    return resolve_chain_recursive(alloc, db, strategy, strategy_len, out, out_cap, out_len, 0);
}

static hu_error_t resolve_chain_recursive(hu_allocator_t *alloc, sqlite3 *db,
    const char *strategy, size_t strategy_len,
    char *out, size_t out_cap, size_t *out_len,
    int depth) {
    if (depth > 3) {
        if (strategy_len >= out_cap)
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(out, strategy, strategy_len);
        out[strategy_len] = '\0';
        *out_len = strategy_len;
        return HU_OK;
    }

    size_t written = 0;
    const char *p = strategy;
    const char *end = strategy + strategy_len;

    while (p < end && written < out_cap - 1) {
        const char *next = (const char *)memchr(p, 's', (size_t)(end - p));
        if (!next) {
            size_t copy = (size_t)(end - p);
            if (copy > out_cap - 1 - written)
                copy = out_cap - 1 - written;
            memcpy(out + written, p, copy);
            written += copy;
            break;
        }
        if ((size_t)(end - next) < SKILL_PREFIX_LEN ||
            memcmp(next, SKILL_PREFIX, SKILL_PREFIX_LEN) != 0) {
            size_t copy = (size_t)(next - p + 1);
            if (copy > out_cap - 1 - written)
                copy = out_cap - 1 - written;
            memcpy(out + written, p, copy);
            written += copy;
            p = next + 1;
            continue;
        }

        if (next > p) {
            size_t copy = (size_t)(next - p);
            if (copy > out_cap - 1 - written)
                copy = out_cap - 1 - written;
            memcpy(out + written, p, copy);
            written += copy;
        }

        const char *name_start = next + SKILL_PREFIX_LEN;
        const char *name_end = name_start;
        while (name_end < end && *name_end != ' ' && *name_end != '\t' && *name_end != '\n' &&
               *name_end != '\r' && *name_end != ',' && *name_end != '.')
            name_end++;

        size_t name_len = (size_t)(name_end - name_start);
        if (name_len == 0) {
            p = next + 1;
            continue;
        }

        hu_skill_t ref = {0};
        hu_error_t err = hu_skill_get_by_name(alloc, db, name_start, name_len, &ref);
        if (err != HU_OK) {
            size_t span = (size_t)(name_end - next);
            if (span > out_cap - 1 - written)
                span = out_cap - 1 - written;
            memcpy(out + written, next, span);
            written += span;
            p = name_end;
            continue;
        }

        char expanded[2048];
        size_t exp_len = 0;
        err = resolve_chain_recursive(alloc, db, ref.strategy, ref.strategy_len,
            expanded, sizeof(expanded), &exp_len, depth + 1);
        if (err != HU_OK)
            return err;

        if (exp_len > out_cap - 1 - written)
            exp_len = out_cap - 1 - written;
        memcpy(out + written, expanded, exp_len);
        written += exp_len;

        p = name_end;
    }

    out[written] = '\0';
    *out_len = written;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
