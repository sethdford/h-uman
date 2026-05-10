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
                                    int64_t stale_after_secs, hu_diag_item_t **items,
                                    size_t *count, size_t *cap);

/* Diagnose the response verifier (W4) by reading the metrics heartbeat file
 * (~/.human/verifier_metrics.json) the daemon flushes every minute. Reports
 * total runs, total/supported/flagged claims, the flagged-rate, and warns
 * when the heartbeat is older than `stale_after_secs` (suggests the daemon
 * is offline or never reached its first flush) or when the flagged rate
 * exceeds `flagged_warn_rate` in the closed interval [0,1]. */
hu_error_t hu_doctor_check_verifier(hu_allocator_t *alloc, int64_t now_epoch,
                                    int64_t stale_after_secs, double flagged_warn_rate,
                                    hu_diag_item_t **items, size_t *count, size_t *cap);

#endif /* HU_DOCTOR_H */
