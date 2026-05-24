/* Spec 2026-05-19 self-model-scaffold — Phase A.
 *
 * Per-turn behavioral observation ring buffer. See
 * include/human/agent/self_model.h for the contract.
 *
 * Privacy invariant (AC-SM-7): this TU stores hashes, sizes, enum
 * codes, and timestamps only. It MUST NEVER ingest user message
 * content, agent response bodies, tool arguments, or any other
 * content-carrying string fields. Pinned by the grep test in
 * tests/test_self_model_no_content_capture.c (Phase B).
 *
 * Build gating per `.claude/rules/test-source-gate-symmetry.md`:
 * source body is wrapped in `#ifdef HU_ENABLE_SELF_MODEL`; the `#else`
 * branch provides bare-return stubs so the public symbols always
 * resolve at link time and callers do not need conditional
 * compilation. AC-SM-6 (zero-cost when OFF) is satisfied because the
 * stubs compile to a handful of bytes per function.
 */

#include "human/agent/self_model.h"
#include "human/core/log.h"

#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#ifdef HU_ENABLE_SELF_MODEL

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static int self_model_log_is_initialized(const hu_agent_behavior_log_t *log) {
    return log != NULL && log->records != NULL && log->capacity > 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

hu_error_t hu_agent_behavior_log_init(hu_agent_behavior_log_t *log, const hu_allocator_t *allocator,
                                      size_t capacity) {
    if (log == NULL || allocator == NULL || allocator->alloc == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    /* Always zero the struct first so partial-init paths leave a safe
     * resting state (records=NULL, capacity=0, head=0). */
    memset(log, 0, sizeof(*log));

    if (capacity == 0) {
        capacity = HU_AGENT_BEHAVIOR_LOG_DEFAULT_CAPACITY;
    }
    if (capacity > HU_AGENT_BEHAVIOR_LOG_MAX_CAPACITY) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t slab_bytes = capacity * sizeof(hu_agent_behavior_record_t);
    hu_agent_behavior_record_t *slab =
        (hu_agent_behavior_record_t *)allocator->alloc(allocator->ctx, slab_bytes);
    if (slab == NULL) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(slab, 0, slab_bytes);

    log->records = slab;
    log->capacity = capacity;
    log->head = 0;
    log->allocator = *allocator;
    return HU_OK;
}

void hu_agent_behavior_log_destroy(hu_agent_behavior_log_t *log) {
    if (log == NULL) {
        return;
    }
    if (log->records != NULL && log->allocator.free != NULL) {
        size_t slab_bytes = log->capacity * sizeof(hu_agent_behavior_record_t);
        log->allocator.free(log->allocator.ctx, log->records, slab_bytes);
    }
    memset(log, 0, sizeof(*log));
}

hu_error_t hu_agent_behavior_log_record(hu_agent_behavior_log_t *log,
                                        const hu_agent_behavior_record_t *rec) {
    /* Silently tolerant of NULL / uninitialized log. The hot path must
     * not punish callers for a misconfigured subsystem; the operator-
     * visible signal lives in the one-shot init log line (Phase B). */
    if (!self_model_log_is_initialized(log) || rec == NULL) {
        return HU_OK;
    }
    /* Zero-alloc fast path: value-copy into slot, increment monotonic
     * head. Wrap is implicit via head % capacity at snapshot time. */
    size_t slot = log->head % log->capacity;
    log->records[slot] = *rec;
    log->head++;
    return HU_OK;
}

hu_error_t hu_agent_behavior_log_snapshot(const hu_agent_behavior_log_t *log,
                                          hu_agent_behavior_record_t *out, size_t max_out,
                                          size_t *out_count) {
    if (out_count == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0;
    if (!self_model_log_is_initialized(log) || out == NULL || max_out == 0) {
        return HU_OK;
    }

    /* Number of records actually present in the buffer. Either head
     * (if we have not yet wrapped) or capacity (if we have). */
    size_t available = (log->head < log->capacity) ? log->head : log->capacity;
    size_t n = (max_out < available) ? max_out : available;
    if (n == 0) {
        return HU_OK;
    }

    if (log->head <= log->capacity) {
        /* No wrap yet — slots [0 .. head) hold the records in order.
         * Copy the last n of those. */
        size_t start = log->head - n;
        memcpy(out, &log->records[start], n * sizeof(hu_agent_behavior_record_t));
    } else {
        /* Wrapped. The physically-oldest record sits at slot
         * (head % capacity); records are chronological from there for
         * `capacity` slots. We want the most recent n of those, in
         * chronological order. Walk forward from
         * ((head - n) % capacity) for n records. */
        size_t start = (log->head - n) % log->capacity;
        if (start + n <= log->capacity) {
            memcpy(out, &log->records[start], n * sizeof(hu_agent_behavior_record_t));
        } else {
            size_t first = log->capacity - start;
            memcpy(out, &log->records[start], first * sizeof(hu_agent_behavior_record_t));
            memcpy(&out[first], &log->records[0], (n - first) * sizeof(hu_agent_behavior_record_t));
        }
    }
    *out_count = n;
    return HU_OK;
}

size_t hu_agent_behavior_log_total_records(const hu_agent_behavior_log_t *log) {
    if (log == NULL) {
        return 0;
    }
    return log->head;
}

/* ===================================================================
 * Phase C — Periodic aggregation + drift signal (HU_ENABLE_SQLITE only)
 * =================================================================== */

#ifdef HU_ENABLE_SQLITE

#define HU_AGENT_SELF_OBSERVATION_WINDOW_MAX 1024

static hu_error_t self_model_run_ddl(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return HU_ERR_IO;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_OK) {
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_agent_self_model_init_tables(sqlite3 *db) {
    if (db == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    /* Six idempotent DDL statements (two tables, four indexes /
     * supporting). Each one runs in its own prepare/step pair so the
     * function compiles cleanly without resorting to the variadic
     * sqlite3 batch helper. */
    static const char *const stmts[] = {
        "CREATE TABLE IF NOT EXISTS agent_self_observations("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "window_start_ts_ms INTEGER NOT NULL,"
        "window_end_ts_ms INTEGER NOT NULL,"
        "n_turns INTEGER NOT NULL,"
        "response_length_mean REAL,"
        "response_length_stddev REAL,"
        "tool_selection_entropy REAL,"
        "emotion_dist_blob BLOB,"
        "latency_p50_ms INTEGER,"
        "latency_p95_ms INTEGER)",
        "CREATE INDEX IF NOT EXISTS idx_self_obs_window_end "
        "ON agent_self_observations(window_end_ts_ms)",
        "CREATE TABLE IF NOT EXISTS agent_self_concerns("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "observation_id INTEGER NOT NULL,"
        "dimension TEXT NOT NULL,"
        "magnitude_sigma REAL NOT NULL,"
        "window_n_turns INTEGER NOT NULL,"
        "created_ts_ms INTEGER NOT NULL,"
        "FOREIGN KEY (observation_id) REFERENCES agent_self_observations(id))",
        "CREATE INDEX IF NOT EXISTS idx_self_concerns_observation "
        "ON agent_self_concerns(observation_id)",
    };
    for (size_t i = 0; i < sizeof(stmts) / sizeof(stmts[0]); i++) {
        hu_error_t e = self_model_run_ddl(db, stmts[i]);
        if (e != HU_OK) {
            return e;
        }
    }
    return HU_OK;
}

/* Compare uint32_t for qsort (ascending). */
static int self_model_cmp_u32(const void *a, const void *b) {
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    if (va < vb)
        return -1;
    if (va > vb)
        return 1;
    return 0;
}

/* Percentile from a sorted u32 array — nearest-rank method. */
static uint32_t self_model_percentile_u32(const uint32_t *sorted, size_t n, double pct) {
    if (n == 0) {
        return 0;
    }
    if (n == 1) {
        return sorted[0];
    }
    double idx = pct * (double)(n - 1);
    size_t i = (size_t)idx;
    if (i >= n) {
        i = n - 1;
    }
    return sorted[i];
}

/* Shannon entropy in bits over a frequency histogram of tool-sequence
 * hashes. Linear scan; n is bounded by the ring capacity. Allocations
 * are inline up to 64 distinct buckets; beyond that we fall through to
 * heap calloc — typical traffic only sees a handful of distinct tool
 * sequences. */
static double self_model_shannon_entropy_u32(const uint32_t *values, size_t n) {
    if (n == 0) {
        return 0.0;
    }
    uint32_t inline_keys[64];
    uint32_t inline_counts[64];
    size_t bucket_count = 0;
    uint32_t *keys = inline_keys;
    uint32_t *counts = inline_counts;
    size_t cap = 64;
    bool heap = false;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = values[i];
        size_t found = SIZE_MAX;
        for (size_t k = 0; k < bucket_count; k++) {
            if (keys[k] == v) {
                found = k;
                break;
            }
        }
        if (found == SIZE_MAX) {
            if (bucket_count == cap) {
                size_t new_cap = cap * 2;
                uint32_t *new_keys = (uint32_t *)calloc(new_cap, sizeof(uint32_t));
                uint32_t *new_counts = (uint32_t *)calloc(new_cap, sizeof(uint32_t));
                if (new_keys == NULL || new_counts == NULL) {
                    free(new_keys);
                    free(new_counts);
                    if (heap) {
                        free(keys);
                        free(counts);
                    }
                    return 0.0;
                }
                memcpy(new_keys, keys, bucket_count * sizeof(uint32_t));
                memcpy(new_counts, counts, bucket_count * sizeof(uint32_t));
                if (heap) {
                    free(keys);
                    free(counts);
                }
                keys = new_keys;
                counts = new_counts;
                cap = new_cap;
                heap = true;
            }
            keys[bucket_count] = v;
            counts[bucket_count] = 1;
            bucket_count++;
        } else {
            counts[found]++;
        }
    }
    double h = 0.0;
    double inv_n = 1.0 / (double)n;
    for (size_t k = 0; k < bucket_count; k++) {
        double p = (double)counts[k] * inv_n;
        if (p > 0.0) {
            h -= p * (log(p) / log(2.0));
        }
    }
    if (heap) {
        free(keys);
        free(counts);
    }
    return h;
}

hu_error_t hu_agent_self_model_compute_and_insert_observation(
    sqlite3 *db, const hu_agent_behavior_log_t *log, size_t window_n, int64_t now_ts_ms,
    int64_t *out_observation_id, hu_agent_self_observation_t *out_observation) {
    if (db == NULL || log == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (window_n == 0) {
        window_n = HU_AGENT_SELF_OBSERVATION_WINDOW_MAX;
    }
    if (window_n > HU_AGENT_SELF_OBSERVATION_WINDOW_MAX) {
        window_n = HU_AGENT_SELF_OBSERVATION_WINDOW_MAX;
    }

    hu_agent_behavior_record_t recs[HU_AGENT_SELF_OBSERVATION_WINDOW_MAX];
    size_t recs_count = 0;
    hu_error_t serr = hu_agent_behavior_log_snapshot(log, recs, window_n, &recs_count);
    if (serr != HU_OK) {
        return serr;
    }
    if (recs_count == 0) {
        if (out_observation_id != NULL) {
            *out_observation_id = 0;
        }
        if (out_observation != NULL) {
            memset(out_observation, 0, sizeof(*out_observation));
        }
        return HU_OK;
    }

    double sum = 0.0;
    double sum_sq = 0.0;
    uint32_t latencies[HU_AGENT_SELF_OBSERVATION_WINDOW_MAX];
    uint32_t tool_seqs[HU_AGENT_SELF_OBSERVATION_WINDOW_MAX];
    uint32_t emotion_dist[5] = {0};
    int64_t win_start = recs[0].timestamp_utc_ms;
    int64_t win_end = recs[0].timestamp_utc_ms;
    for (size_t i = 0; i < recs_count; i++) {
        double x = (double)recs[i].response_length_chars;
        sum += x;
        sum_sq += x * x;
        latencies[i] = recs[i].response_latency_ms;
        tool_seqs[i] = recs[i].tool_sequence_hash;
        if (recs[i].emotional_register < 5) {
            emotion_dist[recs[i].emotional_register]++;
        }
        if (recs[i].timestamp_utc_ms < win_start) {
            win_start = recs[i].timestamp_utc_ms;
        }
        if (recs[i].timestamp_utc_ms > win_end) {
            win_end = recs[i].timestamp_utc_ms;
        }
    }
    double mean = sum / (double)recs_count;
    double variance = (sum_sq / (double)recs_count) - (mean * mean);
    if (variance < 0.0) {
        variance = 0.0;
    }
    double stddev = sqrt(variance);
    double entropy = self_model_shannon_entropy_u32(tool_seqs, recs_count);
    qsort(latencies, recs_count, sizeof(uint32_t), self_model_cmp_u32);
    uint32_t p50 = self_model_percentile_u32(latencies, recs_count, 0.50);
    uint32_t p95 = self_model_percentile_u32(latencies, recs_count, 0.95);

    hu_agent_self_observation_t obs;
    memset(&obs, 0, sizeof(obs));
    obs.window_start_ts_ms = win_start;
    obs.window_end_ts_ms = (now_ts_ms > 0) ? now_ts_ms : win_end;
    obs.n_turns = (uint32_t)recs_count;
    obs.response_length_mean = mean;
    obs.response_length_stddev = stddev;
    obs.tool_selection_entropy = entropy;
    memcpy(obs.emotion_dist, emotion_dist, sizeof(emotion_dist));
    obs.latency_p50_ms = p50;
    obs.latency_p95_ms = p95;

    sqlite3_stmt *stmt = NULL;
    const char *sql_ins = "INSERT INTO agent_self_observations("
                          "window_start_ts_ms, window_end_ts_ms, n_turns,"
                          "response_length_mean, response_length_stddev,"
                          "tool_selection_entropy, emotion_dist_blob,"
                          "latency_p50_ms, latency_p95_ms) VALUES(?,?,?,?,?,?,?,?,?)";
    int rc = sqlite3_prepare_v2(db, sql_ins, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return HU_ERR_IO;
    }
    sqlite3_bind_int64(stmt, 1, obs.window_start_ts_ms);
    sqlite3_bind_int64(stmt, 2, obs.window_end_ts_ms);
    sqlite3_bind_int64(stmt, 3, (int64_t)obs.n_turns);
    sqlite3_bind_double(stmt, 4, obs.response_length_mean);
    sqlite3_bind_double(stmt, 5, obs.response_length_stddev);
    sqlite3_bind_double(stmt, 6, obs.tool_selection_entropy);
    sqlite3_bind_blob(stmt, 7, obs.emotion_dist, (int)sizeof(obs.emotion_dist), SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 8, (int64_t)obs.latency_p50_ms);
    sqlite3_bind_int64(stmt, 9, (int64_t)obs.latency_p95_ms);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return HU_ERR_IO;
    }

    int64_t obs_id = sqlite3_last_insert_rowid(db);
    if (out_observation_id != NULL) {
        *out_observation_id = obs_id;
    }
    if (out_observation != NULL) {
        *out_observation = obs;
    }
    return HU_OK;
}

static hu_error_t self_model_insert_concern(sqlite3 *db, int64_t observation_id,
                                            const char *dimension, double magnitude_sigma,
                                            uint32_t window_n_turns, int64_t created_ts_ms) {
    sqlite3_stmt *stmt = NULL;
    const char *sql_ins = "INSERT INTO agent_self_concerns("
                          "observation_id, dimension, magnitude_sigma, window_n_turns, "
                          "created_ts_ms) VALUES(?,?,?,?,?)";
    int rc = sqlite3_prepare_v2(db, sql_ins, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return HU_ERR_IO;
    }
    sqlite3_bind_int64(stmt, 1, observation_id);
    sqlite3_bind_text(stmt, 2, dimension, (int)strlen(dimension), SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, magnitude_sigma);
    sqlite3_bind_int64(stmt, 4, (int64_t)window_n_turns);
    sqlite3_bind_int64(stmt, 5, created_ts_ms);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t
hu_agent_self_model_detect_drift(sqlite3 *db, const hu_agent_self_observation_t *observation,
                                 int64_t observation_id, double baseline_response_length_mean,
                                 double baseline_response_length_stddev, uint32_t baseline_n,
                                 double drift_threshold_sigma, uint32_t min_baseline_n,
                                 int64_t now_ts_ms, uint32_t *out_concerns_inserted) {
    if (out_concerns_inserted != NULL) {
        *out_concerns_inserted = 0;
    }
    if (db == NULL || observation == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (baseline_n < min_baseline_n) {
        return HU_OK;
    }
    if (drift_threshold_sigma <= 0.0) {
        drift_threshold_sigma = HU_AGENT_SELF_MODEL_DRIFT_THRESHOLD_SIGMA_DEFAULT;
    }

    uint32_t inserted = 0;

    if (baseline_response_length_stddev > 0.0) {
        double sigma = (observation->response_length_mean - baseline_response_length_mean) /
                       baseline_response_length_stddev;
        double abs_sigma = sigma < 0.0 ? -sigma : sigma;
        if (abs_sigma >= drift_threshold_sigma) {
            hu_error_t ie = self_model_insert_concern(db, observation_id, "response_length", sigma,
                                                      observation->n_turns, now_ts_ms);
            if (ie != HU_OK) {
                return ie;
            }
            inserted++;
        }
    }

    if (out_concerns_inserted != NULL) {
        *out_concerns_inserted = inserted;
    }
    return HU_OK;
}

static atomic_bool g_warned_self_obs_disabled = false;
static atomic_bool g_warned_self_obs_enabled = false;

#if HU_IS_TEST
void hu_daemon_tick_self_observation_aggregate_reset_warn_guards_for_test(void) {
    atomic_store(&g_warned_self_obs_disabled, false);
    atomic_store(&g_warned_self_obs_enabled, false);
}
#endif

hu_error_t hu_daemon_tick_self_observation_aggregate(
    sqlite3 *db, const hu_agent_behavior_log_t *log, int64_t now_ts_ms,
    int64_t *last_run_ts_ms_inout, size_t *last_run_total_records_inout,
    int64_t aggregate_every_n_turns, int64_t aggregate_every_sec,
    double baseline_response_length_mean, double baseline_response_length_stddev,
    uint32_t baseline_n, double drift_threshold_sigma, uint32_t min_baseline_n) {
    if (db == NULL || log == NULL || last_run_ts_ms_inout == NULL ||
        last_run_total_records_inout == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (aggregate_every_n_turns <= 0 && aggregate_every_sec <= 0) {
        hu_log_info_once(
            &g_warned_self_obs_disabled, "daemon", NULL,
            "self_model aggregation tick disabled — set self_model.aggregate_every_n_turns "
            "and/or self_model.aggregate_every_sec in config.json to enable");
        return HU_OK;
    }

    if (aggregate_every_n_turns <= 0) {
        aggregate_every_n_turns = (int64_t)HU_AGENT_SELF_OBSERVATION_AGG_DEFAULT_TURNS;
    }
    if (aggregate_every_sec <= 0) {
        aggregate_every_sec = (int64_t)HU_AGENT_SELF_OBSERVATION_AGG_DEFAULT_SEC;
    }

    hu_log_info_once(&g_warned_self_obs_enabled, "daemon", NULL,
                     "self_model aggregation tick enabled — every %lld turns OR %lld seconds "
                     "(drift_threshold_sigma=%.2f, min_baseline_n=%u)",
                     (long long)aggregate_every_n_turns, (long long)aggregate_every_sec,
                     drift_threshold_sigma, min_baseline_n);

    size_t now_total = hu_agent_behavior_log_total_records(log);
    int64_t interval_ms = aggregate_every_sec * 1000;

    bool by_turns = (now_total >= *last_run_total_records_inout + (size_t)aggregate_every_n_turns);
    bool by_time = (*last_run_ts_ms_inout > 0 && now_ts_ms - *last_run_ts_ms_inout >= interval_ms);
    bool first_tick = (*last_run_ts_ms_inout == 0 && now_total > 0);

    if (!(by_turns || by_time || first_tick)) {
        return HU_OK;
    }

    int64_t obs_id = 0;
    hu_agent_self_observation_t obs;
    memset(&obs, 0, sizeof(obs));
    hu_error_t err = hu_agent_self_model_compute_and_insert_observation(
        db, log, (size_t)aggregate_every_n_turns, now_ts_ms, &obs_id, &obs);

    *last_run_ts_ms_inout = now_ts_ms;
    *last_run_total_records_inout = now_total;

    if (err != HU_OK || obs_id == 0) {
        return err;
    }

    uint32_t concerns_inserted = 0;
    (void)hu_agent_self_model_detect_drift(
        db, &obs, obs_id, baseline_response_length_mean, baseline_response_length_stddev,
        baseline_n, drift_threshold_sigma, min_baseline_n, now_ts_ms, &concerns_inserted);
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */

#else /* !HU_ENABLE_SELF_MODEL */

/* OFF-variant stubs. Match `.claude/rules/test-source-gate-symmetry.md`'s
 * "internal #ifdef wrap with stub runner" pattern: keep the symbols
 * resolvable at link time so callers do not need conditional compilation.
 * Bare returns; no allocation, no I/O.
 *
 * `(void)` casts suppress -Wunused-parameter without restating the
 * struct shapes. */

hu_error_t hu_agent_behavior_log_init(hu_agent_behavior_log_t *log, const hu_allocator_t *allocator,
                                      size_t capacity) {
    (void)allocator;
    (void)capacity;
    if (log != NULL) {
        memset(log, 0, sizeof(*log));
    }
    return HU_OK;
}

void hu_agent_behavior_log_destroy(hu_agent_behavior_log_t *log) {
    if (log != NULL) {
        memset(log, 0, sizeof(*log));
    }
}

hu_error_t hu_agent_behavior_log_record(hu_agent_behavior_log_t *log,
                                        const hu_agent_behavior_record_t *rec) {
    (void)log;
    (void)rec;
    return HU_OK;
}

hu_error_t hu_agent_behavior_log_snapshot(const hu_agent_behavior_log_t *log,
                                          hu_agent_behavior_record_t *out, size_t max_out,
                                          size_t *out_count) {
    (void)log;
    (void)out;
    (void)max_out;
    if (out_count != NULL) {
        *out_count = 0;
    }
    return HU_OK;
}

size_t hu_agent_behavior_log_total_records(const hu_agent_behavior_log_t *log) {
    (void)log;
    return 0;
}

#endif /* HU_ENABLE_SELF_MODEL */
