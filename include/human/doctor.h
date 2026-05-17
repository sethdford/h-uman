#ifndef HU_DOCTOR_H
#define HU_DOCTOR_H

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

typedef enum hu_diag_severity {
    HU_DIAG_OK,
    HU_DIAG_WARN,
    HU_DIAG_ERR,
} hu_diag_severity_t;

typedef struct hu_diag_item {
    hu_diag_severity_t severity;
    const char *category;
    const char *message;
} hu_diag_item_t;

/* Parse df -m output; return available MB or 0 on parse fail. */
unsigned long hu_doctor_parse_df_available_mb(const char *df_output, size_t len);

/* Truncate string for display at valid UTF-8 boundary. Caller must free. */
hu_error_t hu_doctor_truncate_for_display(hu_allocator_t *alloc, const char *s, size_t len,
                                          size_t max_len, char **out);

/* Run config semantics checks; append items. */
hu_error_t hu_doctor_check_config_semantics(hu_allocator_t *alloc, const hu_config_t *cfg,
                                            hu_diag_item_t **items, size_t *count);

/* Check security posture: exec env sanitization, safe-bin allowlist,
 * sandbox availability, HTTPS enforcement. */
hu_error_t hu_doctor_check_security(hu_allocator_t *alloc, hu_diag_item_t **items, size_t *count,
                                    size_t *cap);

/* Check memory backend health (SQLite integrity, disk space). */
hu_error_t hu_doctor_check_memory_health(hu_allocator_t *alloc, const hu_config_t *cfg,
                                         hu_diag_item_t **items, size_t *count, size_t *cap);

/* Check skill registry and installed skills. */
hu_error_t hu_doctor_check_skills(hu_allocator_t *alloc, hu_diag_item_t **items, size_t *count,
                                  size_t *cap);

/* Diagnose the iMessage channel: chat.db readability, imsg CLI presence on PATH,
 * poll-status file freshness, and circuit-breaker state. Always available so
 * non-iMessage builds get a single OK/SKIP line rather than a missing symbol.
 *
 * `now_epoch` is the current wall-clock time used for staleness comparisons;
 * the caller passes it to keep this function pure with respect to time. The
 * `stale_after_secs` argument controls the WARN threshold for "no successful
 * poll in the last N seconds" (e.g. 600 for 10 minutes). */
hu_error_t hu_doctor_check_imessage(hu_allocator_t *alloc, int64_t now_epoch,
                                    int64_t stale_after_secs, hu_diag_item_t **items, size_t *count,
                                    size_t *cap);

/* Diagnose the response verifier (W4) by reading the metrics heartbeat file
 * (~/.human/verifier_metrics.json) the daemon flushes every minute. Reports
 * total runs, total/supported/flagged claims, the flagged-rate, and warns
 * when the heartbeat is older than `stale_after_secs` (suggests the daemon
 * is offline or never reached its first flush) or when the flagged rate
 * exceeds `flagged_warn_rate` in the closed interval [0,1]. */
hu_error_t hu_doctor_check_verifier(hu_allocator_t *alloc, int64_t now_epoch,
                                    int64_t stale_after_secs, double flagged_warn_rate,
                                    hu_diag_item_t **items, size_t *count, size_t *cap);

/* Parse ~/.human/scheduler.status JSON body.
 *
 * Deprecated for new callers: use `hu_scheduler_status_parse_json` from
 * `human/agent/scheduler_status_json.h` so non-doctor modules (e.g. ML CLI) do not
 * depend on this header. This symbol remains as a thin compatibility wrapper. */
hu_error_t hu_doctor_parse_scheduler_status_json(const char *json, unsigned long long *jobs_pending,
                                                 unsigned long long *jobs_completed_today,
                                                 long long *battery_pct, char *on_ac_power_text,
                                                 size_t on_ac_power_cap, long long *updated_epoch);

/* Read ~/.human/scheduler.status (written by the daemon after each W14 tick)
 * and report pending/completed counts + freshness vs `now_epoch`. */
hu_error_t hu_doctor_check_scheduler(hu_allocator_t *alloc, int64_t now_epoch,
                                     int64_t stale_after_secs, hu_diag_item_t **items,
                                     size_t *count, size_t *cap);

/* Operational hints for response_guard / empty-reply triage (no secrets). */
hu_error_t hu_doctor_check_response_pipeline(hu_allocator_t *alloc, hu_diag_item_t **items,
                                             size_t *count, size_t *cap);

/* US-9.4 install-readiness gate. One predicate, four sub-checks, each
 * reading PRIMARY EVIDENCE on the live filesystem (not a cached flag):
 *
 *   1. binary       — running executable resolves and stats as a regular file.
 *                     Linux: readlink /proc/self/exe; macOS: _NSGetExecutablePath
 *                     + realpath. Under HU_IS_TEST, honors $HU_TEST_BINARY_PATH.
 *                     Category: "binary".
 *   2. config_dir   — ~/.human/ (or $HU_STATE_DIR override) stats as a directory.
 *                     Category: "config_dir".
 *   3. channel      — cfg->channels.channel_config_keys[] contains at least
 *                     one entry with a non-empty key and non-zero count.
 *                     Category: "channel".
 *   4. persona      — hu_persona_load(cfg->agent.persona) succeeds; file
 *                     present + parses as JSON, or embedded persona resolves.
 *                     Category: "persona".
 *
 * Returns HU_OK iff all four sub-checks are HU_DIAG_OK. Returns
 * HU_ERR_NOT_FOUND if any sub-check is HU_DIAG_ERR (the predicate appends
 * all four items either way — it does NOT short-circuit, so the user sees
 * every red line in one run). Returns HU_ERR_OUT_OF_MEMORY on alloc fail,
 * HU_ERR_INVALID_ARGUMENT on NULL alloc/items/count/cap. `cfg` may be NULL
 * — callers that failed to load the config still get a single
 * config_dir/channel/persona red line rather than a crash.
 */
hu_error_t hu_doctor_check_install(hu_allocator_t *alloc, const hu_config_t *cfg,
                                   hu_diag_item_t **items, size_t *count, size_t *cap);

#endif /* HU_DOCTOR_H */
