#include "human/core/error.h"
#include "human/intelligence/meta_learning.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#define HU_META_KEY_CONFIDENCE "meta_confidence_threshold"
#define HU_META_KEY_REFINEMENT "meta_refinement_weeks"
#define HU_META_KEY_DISCOVERY "meta_discovery_min_feedback"

#define HU_META_DEFAULT_CONFIDENCE 0.7
#define HU_META_DEFAULT_REFINEMENT 1
#define HU_META_DEFAULT_DISCOVERY 3

#define HU_META_MIN_THRESHOLD 0.5
#define HU_META_MAX_THRESHOLD 0.95
#define HU_META_DELTA 0.05

static hu_error_t kv_get_double(sqlite3 *db, const char *key, double *out) {
    const char *sql = "SELECT value FROM kv WHERE key = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return HU_ERR_NOT_FOUND;
    }
    const char *val = (const char *)sqlite3_column_text(stmt, 0);
    if (val)
        *out = strtod(val, NULL);
    sqlite3_finalize(stmt);
    return HU_OK;
}

static hu_error_t kv_get_int(sqlite3 *db, const char *key, int *out) {
    const char *sql = "SELECT value FROM kv WHERE key = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return HU_ERR_NOT_FOUND;
    }
    const char *val = (const char *)sqlite3_column_text(stmt, 0);
    if (val)
        *out = atoi(val);
    sqlite3_finalize(stmt);
    return HU_OK;
}

static hu_error_t kv_set(sqlite3 *db, const char *key, const char *value) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%s", value);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return HU_ERR_INVALID_ARGUMENT;

    const char *sql = "INSERT OR REPLACE INTO kv (key, value) VALUES (?1, ?2)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, buf, n, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    return HU_OK;
}

hu_error_t hu_meta_learning_load(sqlite3 *db, hu_meta_params_t *out) {
    if (!db || !out)
        return HU_ERR_INVALID_ARGUMENT;

    out->default_confidence_threshold = HU_META_DEFAULT_CONFIDENCE;
    out->refinement_frequency_weeks = HU_META_DEFAULT_REFINEMENT;
    out->discovery_min_feedback_count = HU_META_DEFAULT_DISCOVERY;

    double d;
    if (kv_get_double(db, HU_META_KEY_CONFIDENCE, &d) == HU_OK)
        out->default_confidence_threshold = d;
    int i;
    if (kv_get_int(db, HU_META_KEY_REFINEMENT, &i) == HU_OK)
        out->refinement_frequency_weeks = i;
    if (kv_get_int(db, HU_META_KEY_DISCOVERY, &i) == HU_OK)
        out->discovery_min_feedback_count = i;

    return HU_OK;
}

hu_error_t hu_meta_learning_update(sqlite3 *db, const hu_meta_params_t *params) {
    if (!db || !params)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.4f", params->default_confidence_threshold);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err = kv_set(db, HU_META_KEY_CONFIDENCE, buf);
    if (err != HU_OK)
        return err;

    n = snprintf(buf, sizeof(buf), "%d", params->refinement_frequency_weeks);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return HU_ERR_INVALID_ARGUMENT;
    err = kv_set(db, HU_META_KEY_REFINEMENT, buf);
    if (err != HU_OK)
        return err;

    n = snprintf(buf, sizeof(buf), "%d", params->discovery_min_feedback_count);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return HU_ERR_INVALID_ARGUMENT;
    err = kv_set(db, HU_META_KEY_DISCOVERY, buf);
    return err;
}

hu_error_t hu_meta_learning_optimize(sqlite3 *db, hu_meta_params_t *out) {
    if (!db || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_meta_learning_load(db, out);
    if (err != HU_OK)
        return err;

    const char *sql =
        "SELECT AVG(success_rate) FROM skills WHERE retired = 0 AND attempts >= ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int(stmt, 1, out->discovery_min_feedback_count);
    rc = sqlite3_step(stmt);
    double avg_sr = 0.5;
    if (rc == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        avg_sr = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);

    double th = out->default_confidence_threshold;
    if (avg_sr > 0.7)
        th -= HU_META_DELTA;
    else if (avg_sr < 0.4)
        th += HU_META_DELTA;

    if (th < HU_META_MIN_THRESHOLD)
        th = HU_META_MIN_THRESHOLD;
    if (th > HU_META_MAX_THRESHOLD)
        th = HU_META_MAX_THRESHOLD;

    out->default_confidence_threshold = th;
    return hu_meta_learning_update(db, out);
}
#endif /* HU_ENABLE_SQLITE */
