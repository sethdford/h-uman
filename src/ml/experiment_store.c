/* Experiment persistence — stores experiment results in SQLite.
 * When HU_ENABLE_SQLITE is not defined, all operations return HU_ERR_NOT_SUPPORTED.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/experiment.h"
#include "human/ml/experiment_store.h"
#include "human/ml/ml.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

struct hu_experiment_store {
    hu_allocator_t *alloc;
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db;
#endif
    const char *path;
};

static int config_hash(const hu_experiment_config_t *config)
{
    if (!config)
        return 0;
    const unsigned char *p = (const unsigned char *)config;
    int h = 5381;
    for (size_t i = 0; i < sizeof(hu_experiment_config_t); i++)
        h = ((h << 5) + h) + (unsigned)p[i];
    return h;
}

static const char *status_to_string(hu_experiment_status_t status)
{
    switch (status) {
    case HU_EXPERIMENT_KEEP:
        return "keep";
    case HU_EXPERIMENT_DISCARD:
        return "discard";
    case HU_EXPERIMENT_CRASH:
        return "crash";
    default:
        return "unknown";
    }
}

#ifdef HU_ENABLE_SQLITE

static const char *create_table_sql =
    "CREATE TABLE IF NOT EXISTS experiments ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "iteration INT,"
    "val_bpb REAL,"
    "peak_memory_mb REAL,"
    "training_seconds REAL,"
    "status TEXT,"
    "description TEXT,"
    "config_hash INT,"
    "created_at TEXT DEFAULT CURRENT_TIMESTAMP)";

hu_error_t hu_experiment_store_open(hu_allocator_t *alloc, const char *db_path,
                                    hu_experiment_store_t **out)
{
    if (!alloc || !db_path || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    hu_experiment_store_t *store =
        alloc->alloc(alloc->ctx, sizeof(hu_experiment_store_t));
    if (!store)
        return HU_ERR_OUT_OF_MEMORY;

    store->alloc = alloc;
    store->db = NULL;
    store->path = db_path;

    int rc = sqlite3_open(db_path, &store->db);
    if (rc != SQLITE_OK) {
        alloc->free(alloc->ctx, store, sizeof(hu_experiment_store_t));
        return HU_ERR_IO;
    }

    rc = sqlite3_exec(store->db, create_table_sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(store->db);
        alloc->free(alloc->ctx, store, sizeof(hu_experiment_store_t));
        return HU_ERR_IO;
    }

    *out = store;
    return HU_OK;
}

hu_error_t hu_experiment_store_save(hu_experiment_store_t *store,
                                    const hu_experiment_result_t *result)
{
    if (!store || !result)
        return HU_ERR_INVALID_ARGUMENT;

    const char *sql =
        "INSERT INTO experiments(iteration,val_bpb,peak_memory_mb,training_seconds,"
        "status,description,config_hash) VALUES(?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    int hash = config_hash(&result->config);
    const char *status = status_to_string(result->status);

    sqlite3_bind_int(stmt, 1, result->iteration);
    sqlite3_bind_double(stmt, 2, result->val_bpb);
    sqlite3_bind_double(stmt, 3, result->peak_memory_mb);
    sqlite3_bind_double(stmt, 4, result->training_seconds);
    sqlite3_bind_text(stmt, 5, status, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, result->description, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 7, hash);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_experiment_store_count(hu_experiment_store_t *store, size_t *count)
{
    if (!store || !count)
        return HU_ERR_INVALID_ARGUMENT;
    *count = 0;

    const char *sql = "SELECT COUNT(*) FROM experiments";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        *count = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return HU_OK;
}

void hu_experiment_store_close(hu_experiment_store_t *store)
{
    if (!store)
        return;
    sqlite3_close(store->db);
    store->alloc->free(store->alloc->ctx, store, sizeof(hu_experiment_store_t));
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_experiment_store_open(hu_allocator_t *alloc, const char *db_path,
                                    hu_experiment_store_t **out)
{
    (void)alloc;
    (void)db_path;
    (void)out;
    if (!alloc || !db_path || !out)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_experiment_store_save(hu_experiment_store_t *store,
                                    const hu_experiment_result_t *result)
{
    (void)store;
    (void)result;
    if (!store || !result)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_experiment_store_count(hu_experiment_store_t *store, size_t *count)
{
    (void)store;
    (void)count;
    if (!store || !count)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_experiment_store_close(hu_experiment_store_t *store)
{
    (void)store;
}

#endif /* HU_ENABLE_SQLITE */
