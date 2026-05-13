#ifndef HU_DOCTOR_FIX_H
#define HU_DOCTOR_FIX_H

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Doctor auto-repair — diagnoses and fixes common configuration issues.
 *
 * Inspired by OpenClaw's `openclaw doctor --fix`. Each fixable issue
 * has a description, a check, and an automated repair action.
 */

typedef struct hu_doctor_fix_result {
    const char *issue;
    const char *action_taken;
    bool fixed;
} hu_doctor_fix_result_t;

/* Run all auto-fixable checks and apply repairs.
 * Returns array of results; caller must free with hu_doctor_fix_results_free. */
hu_error_t hu_doctor_fix(hu_allocator_t *alloc, hu_config_t *cfg, hu_doctor_fix_result_t **results,
                         size_t *result_count);

void hu_doctor_fix_results_free(hu_allocator_t *alloc, hu_doctor_fix_result_t *results,
                                size_t count);

/* Individual fixers (also usable standalone) */

/* Ensure ~/.human/ directory exists. */
hu_error_t hu_doctor_fix_state_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out);

/* Ensure ~/.human/skills/ directory exists. */
hu_error_t hu_doctor_fix_skills_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out);

/* Ensure ~/.human/plugins/ directory exists. */
hu_error_t hu_doctor_fix_plugins_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out);

/* Ensure ~/.human/personas/ directory exists. */
hu_error_t hu_doctor_fix_personas_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out);

/* Write a default config.json if none exists. */
hu_error_t hu_doctor_fix_default_config(hu_allocator_t *alloc, hu_doctor_fix_result_t *out);

/* Diagnose and apply recovery actions for iMessage on macOS.
 *
 * Recovery actions (in order of cost / invasiveness):
 *  - **FDA missing.** chat.db can't be opened. Open System Settings to
 *    Privacy → Full Disk Access via the `x-apple.systempreferences:` deep
 *    link so the user can grant access in one click. Cannot auto-grant
 *    (security boundary), but eliminates the "user doesn't know where
 *    to go" gap that today turns into a silent-daemon support ticket.
 *  - **Stale rowid cache.** `~/.human/imessage.poll_status` has a
 *    timestamp older than 24 hours or is unreadable → delete it so
 *    the next poll rebuilds it from scratch. (Path matches
 *    HU_IMESSAGE_STATUS_FILE in src/channels/imessage_internal.h.)
 *  - **Healthy.** No-op result with action_taken="(no recovery needed)".
 *
 * Non-Apple platforms / test mode return a deterministic "(skipped)"
 * result. Recovery is best-effort and never throws; the only error
 * code the function returns is HU_ERR_INVALID_ARGUMENT when `out` is
 * NULL. All other conditions are reported in the result struct's
 * action_taken / success fields. */
hu_error_t hu_doctor_fix_imessage(hu_allocator_t *alloc, hu_doctor_fix_result_t *out);

#endif /* HU_DOCTOR_FIX_H */
