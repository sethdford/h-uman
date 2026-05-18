#ifndef HU_DOCTOR_H
#define HU_DOCTOR_H

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
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
    /* Optional: machine-readable error class name (e.g. "AUTH", "BUSY",
     * "CANTOPEN", "OTHER", "NONE") for iMessage / channel diagnostic items.
     * NULL when not applicable. Owned by the diag item's allocator. */
    const char *error_class;
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

/* Format a user-actionable diagnostic for an iMessage poll-status JSON blob.
 *
 * `poll_status_json` is the contents of ~/.human/imessage.poll_status (or a
 * minimal synthetic JSON of the same shape, e.g. {"last_error_class":"AUTH"}).
 * The blob's `last_error_class` field is mapped through
 * hu_imessage_error_class_from_name and routed to a class-specific message:
 *
 *   AUTH     -> "Full Disk Access" remediation hint
 *   BUSY     -> "Messages.app may be syncing" hint (MUST NOT mention FDA)
 *   CANTOPEN -> "chat.db not found at <path>" hint
 *   NONE     -> *out is set to NULL (no diagnostic), returns HU_OK
 *   OTHER    -> generic unclassified message via hu_imessage_error_class_name
 *
 * On success, `*out` is either a malloc-style buffer the caller frees via
 * alloc->free (with length strlen(*out) + 1) or NULL when there is no
 * diagnostic to report (NONE class or missing class field). */
hu_error_t hu_imessage_diag_from_poll_status(hu_allocator_t *alloc, const char *poll_status_json,
                                             char **out);

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

/* --- US-43.4: `human doctor --install` install-readiness gate ---
 *
 * Pure-predicate design (see ~/.claude/rules/security-predicate-extraction.md).
 * The decision lives in `hu_doctor_check_install`; the I/O that gathers the
 * state lives in a separate gatherer at the CLI dispatch site. Tests
 * synthesize `hu_doctor_install_state_t` directly and never touch the
 * filesystem (story-mandated test seam). */

typedef enum hu_doctor_install_persona_status {
    HU_DOCTOR_PERSONA_MISSING,
    HU_DOCTOR_PERSONA_PRESENT_VALID,
    HU_DOCTOR_PERSONA_PRESENT_INVALID,
} hu_doctor_install_persona_status_t;

typedef struct hu_doctor_install_state {
    const char *binary_path; /* resolvable invocation path; NULL/empty -> ERR */
    bool config_dir_exists;  /* ~/.human/ stat succeeded as a directory */
    bool channel_paired;     /* >= 1 channel configured */
    hu_doctor_install_persona_status_t persona_status;
} hu_doctor_install_state_t;

/* Append exactly four diagnostic items (binary, config_dir, channel, persona)
 * to `*items`. No short-circuit on failure: even an all-red state still
 * emits four items so callers can render every line. Returns HU_OK in all
 * paths — the failure signal lives in per-item severity (AC-43.4.2 contract).
 *
 * On allocation failure, returns HU_ERR_OUT_OF_MEMORY; partial appends may
 * have landed and remain owned by the caller via the existing
 * doctor_free_diag_items pattern. */
hu_error_t hu_doctor_check_install(hu_allocator_t *alloc, const hu_doctor_install_state_t *state,
                                   hu_diag_item_t **items, size_t *count, size_t *cap);

#endif /* HU_DOCTOR_H */
