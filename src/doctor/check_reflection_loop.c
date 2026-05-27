/* src/doctor/check_reflection_loop.c — T12 doctor check for the
 * M2 reflection loop.
 *
 * Spec: docs/plans/2026-05-26-reflection-loop Task 12 + design.md
 * "Operator health" section. Surfaces the persisted run-row state
 * so operators don't have to grep daemon logs to know whether the
 * subsystem is healthy.
 *
 * Verdict logic (in evaluation order):
 *   1. cfg NULL → NA "no config"
 *   2. !cfg->reflection_loop.enabled → NA "disabled in config"
 *   3. db NULL → NA "subsystem enabled but no db handle for inspection"
 *   4. 0 total runs → NA "enabled but no runs yet (cold start)"
 *   5. last 5 runs all non-ok → FAIL "broken: N consecutive errors"
 *   6. ≥1 ok run in last 7 days → PASS "healthy: N ok / M total in 7d"
 *   7. (fallback) all ok runs older than 7 days → NA "stale: last ok
 *      run was X hours ago"
 *
 * detail_json carries the raw counts so JSON consumers can score
 * without re-querying. */

#include "human/doctor/check_reflection_loop.h"

#include "human/config.h"
#include "human/config_types.h"
#include "human/core/error.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

/* Per-check static buffers per check.h contract — reason and
 * detail_json strings live for the lifetime of the result return. */
static char s_reason_buf[512];
static char s_detail_json_buf[512];

/* Aggregate counts pulled with a single grouped query so we don't
 * spam the db with 4 separate scans. */
typedef struct {
    int total;
    int ok;
    int provider_error;
    int schema_invalid;
    int abandoned;
    int64_t latest_ok_completed_ms; /* 0 if none */
    int consecutive_non_ok;         /* last N rows that are NOT ok, ordered by completed_at */
} run_counts_t;

static void load_run_counts(sqlite3 *db, run_counts_t *out) {
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT status, COUNT(*), MAX(completed_at_ms) "
                           "FROM reflection_runs GROUP BY status",
                           -1, &st, NULL) != SQLITE_OK)
        return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *status = sqlite3_column_text(st, 0);
        int count = sqlite3_column_int(st, 1);
        int64_t max_completed = sqlite3_column_int64(st, 2);
        if (!status)
            continue;
        out->total += count;
        if (strcmp((const char *)status, "ok") == 0) {
            out->ok = count;
            out->latest_ok_completed_ms = max_completed;
        } else if (strcmp((const char *)status, "provider_error") == 0) {
            out->provider_error = count;
        } else if (strcmp((const char *)status, "schema_invalid") == 0) {
            out->schema_invalid = count;
        } else if (strcmp((const char *)status, "abandoned") == 0) {
            out->abandoned = count;
        }
    }
    sqlite3_finalize(st);

    /* Second query: count CONSECUTIVE non-ok runs from the most recent
     * end. Stops counting at the first 'ok' or in_progress. */
    if (sqlite3_prepare_v2(db,
                           "SELECT status FROM reflection_runs "
                           "ORDER BY started_at_ms DESC LIMIT 5",
                           -1, &st, NULL) != SQLITE_OK)
        return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *status = sqlite3_column_text(st, 0);
        if (!status || strcmp((const char *)status, "ok") == 0)
            break;
        if (strcmp((const char *)status, "in_progress") == 0)
            break;
        out->consecutive_non_ok++;
    }
    sqlite3_finalize(st);
}

static hu_doctor_check_result_t check_reflection_loop_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_check_reflection_loop_ctx_t *rctx = (hu_doctor_check_reflection_loop_ctx_t *)ctx;

    if (!rctx || !rctx->cfg) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "reflection_loop check: no config provided to doctor");
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason_buf, NULL};
    }
    if (!rctx->cfg->reflection_loop.enabled) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "reflection_loop disabled in config (reflection.enabled=false). "
                 "Set reflection.enabled=true in config.json to activate the "
                 "periodic pattern-distillation pass.");
        snprintf(s_detail_json_buf, sizeof(s_detail_json_buf), "{\"enabled\":false}");
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason_buf, s_detail_json_buf};
    }
    if (!rctx->db) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "reflection_loop enabled in config but no db handle available to "
                 "doctor for run-history inspection. Run from a daemon-connected "
                 "context, or ensure the SQLite memory backend is configured.");
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason_buf, NULL};
    }

    run_counts_t counts;
    load_run_counts(rctx->db, &counts);

    /* detail_json — always emitted when we got past the early NAs. */
    snprintf(s_detail_json_buf, sizeof(s_detail_json_buf),
             "{\"enabled\":true,\"total\":%d,\"ok\":%d,\"provider_error\":%d,"
             "\"schema_invalid\":%d,\"abandoned\":%d,\"consecutive_non_ok\":%d,"
             "\"latest_ok_completed_ms\":%" PRId64 "}",
             counts.total, counts.ok, counts.provider_error, counts.schema_invalid,
             counts.abandoned, counts.consecutive_non_ok, counts.latest_ok_completed_ms);

    if (counts.total == 0) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "reflection_loop enabled but no runs yet (cold start). First run "
                 "fires when min_interval_hours=%d + idle_threshold_hours=%d both "
                 "satisfied, or after daily_floor_hours=%d.",
                 rctx->cfg->reflection_loop.min_interval_hours,
                 rctx->cfg->reflection_loop.idle_threshold_hours,
                 rctx->cfg->reflection_loop.daily_floor_hours);
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason_buf, s_detail_json_buf};
    }

    /* FAIL: last 5 (or all if <5) runs are all non-ok. */
    if (counts.consecutive_non_ok >= 5 ||
        (counts.consecutive_non_ok == counts.total && counts.total >= 1 && counts.ok == 0)) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "reflection_loop BROKEN: %d consecutive non-ok runs "
                 "(provider_error=%d schema_invalid=%d abandoned=%d). Check provider "
                 "credentials in config.json and verify the model is returning "
                 "valid JSON. Tail ~/.human/logs/daemon.log for [reflection.daemon] "
                 "warn lines.",
                 counts.consecutive_non_ok, counts.provider_error, counts.schema_invalid,
                 counts.abandoned);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason_buf, s_detail_json_buf};
    }

    /* PASS: at least one ok run in the last 7 days. */
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    uint64_t seven_days_ms = 7ULL * 86400000ULL;
    if (counts.ok > 0 && counts.latest_ok_completed_ms > 0 &&
        (uint64_t)counts.latest_ok_completed_ms > now_ms - seven_days_ms) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "reflection_loop healthy: %d ok run%s in last 7d (total runs %d, "
                 "provider_error=%d schema_invalid=%d). Latest ok run %" PRIu64 "h ago.",
                 counts.ok, counts.ok == 1 ? "" : "s", counts.total, counts.provider_error,
                 counts.schema_invalid,
                 (now_ms - (uint64_t)counts.latest_ok_completed_ms) / (3600ULL * 1000ULL));
        return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason_buf, s_detail_json_buf};
    }

    /* Otherwise: stale — runs exist but no recent ok. NA with detail. */
    if (counts.latest_ok_completed_ms > 0) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "reflection_loop stale: last ok run %" PRIu64 "h ago (>7d). "
                 "Subsystem may be paused or interval-gated for too long; check "
                 "config.json min_interval_hours.",
                 (now_ms - (uint64_t)counts.latest_ok_completed_ms) / (3600ULL * 1000ULL));
    } else {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "reflection_loop has %d run%s but none completed ok yet "
                 "(provider_error=%d schema_invalid=%d). Watch the next tick.",
                 counts.total, counts.total == 1 ? "" : "s", counts.provider_error,
                 counts.schema_invalid);
    }
    return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason_buf, s_detail_json_buf};
}

#else /* !HU_ENABLE_SQLITE */

static hu_doctor_check_result_t check_reflection_loop_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    (void)ctx;
    return (hu_doctor_check_result_t){HU_DOCTOR_NA,
                                      "reflection_loop check skipped — built without "
                                      "HU_ENABLE_SQLITE.",
                                      NULL};
}

#endif /* HU_ENABLE_SQLITE */

hu_doctor_check_t hu_doctor_check_reflection_loop = {
    .name = "reflection_loop",
    .description = "M2 reflection-loop health (enabled/cold-start/healthy/broken/stale)",
    .run = check_reflection_loop_run,
    .fix = NULL,
    .user_data = NULL,
};
